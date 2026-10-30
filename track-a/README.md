# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. One pass files each request into that hour. A 64-byte compare finds the spaces and the newline after the timestamp. When parsing finishes, each hour is counting-sorted by endpoint and the three nearest ranks are taken while that hour is still in cache. A writer thread emits hours in order. Transparent huge pages are disabled and glibc is asked to return freed blocks, so the resident set tracks live records rather than old arenas.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Workers equal one per physical core when the run exposes hyperthreads.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

SIMD classification of newlines and a SWAR latency parse. The record is about 31 ms, which is close to streaming the 239 MB once; the remaining gap is bytes touched per line, not the percentile step.
