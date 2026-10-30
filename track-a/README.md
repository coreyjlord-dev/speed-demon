# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. The file is mapped once and walked in 1 MB windows (`MADV_POPULATE_READ`, then `MADV_DONTNEED`) so the log does not stay in RSS. Pass one counts each (hour, endpoint) group. Pass two writes each latency into one exact `uint32` slot. Percentiles are nearest-rank at `(p * n + 99) / 100 - 1`. Output is streamed from a 64 KB buffer after those arrays are released.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Workers equal one per physical core when the run exposes hyperthreads.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

SIMD classification of newlines and a SWAR latency parse. The record is about 31 ms, which is close to streaming the 239 MB once; the remaining gap is bytes touched per line, not the percentile step.
