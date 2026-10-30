# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. Each worker maps the file, takes a newline-aligned slice, and interns endpoints in an open-addressed table keyed by a 64-bit hash. Latencies for a group live in a bump arena, so a hot endpoint does not bounce through the allocator. After the slices merge, each group runs three `nth_element` calls at the nearest-rank indexes `(p * n + 99) / 100 - 1`. Lines come out hour by hour, endpoints in unsigned byte order.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Workers equal one per physical core when the run exposes hyperthreads.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

SIMD classification of newlines and a SWAR latency parse. The record is about 31 ms, which is close to streaming the 239 MB once; the remaining gap is bytes touched per line, not the percentile step.
