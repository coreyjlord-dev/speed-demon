# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. One pass files a 6-byte record into that hour. The latency is taken from the 16 bytes before the newline, and the endpoint is hashed with a masked 64-byte mix; the hash and length are compared before any `memcmp`. Each hour is counting-sorted by endpoint and the three nearest ranks are taken while that hour is still in cache. A writer thread emits hours in order. The run uses every hardware thread up to 12. Transparent huge pages are off, and `mallopt` (`M_TRIM_THRESHOLD`, `M_MMAP_THRESHOLD`) returns freed blocks to the OS. Finished pages of the mapping are dropped; the file is not populated up front.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Workers equal one per physical core when the run exposes hyperthreads.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

SIMD classification of newlines and a SWAR latency parse. The record is about 31 ms, which is close to streaming the 239 MB once; the remaining gap is bytes touched per line, not the percentile step.
