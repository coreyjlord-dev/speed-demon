# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. One pass files a 6-byte record into that hour. The method ends at a branchless SWAR space, the latency is a masked 8-digit horner, and the endpoint hash is a CRC of the first, middle, and last 8 bytes; length and hash are compared before any `memcmp`. Each hour is counting-sorted by endpoint. Groups of up to 96 latencies are insertion-sorted, groups under 512 use three partial selections, and larger groups are radix-sorted. Workers write hours in order, so the run stays at one thread per slice with no extra writer. The cap is 12 threads: the input is time-ordered, and equal byte slices each cover a contiguous run of hours. Transparent huge pages are off, and `mallopt` (`M_TRIM_THRESHOLD`, `M_MMAP_THRESHOLD`) returns freed blocks to the OS. Finished pages of the mapping are dropped; the file is not populated up front.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Thread count is `hardware_concurrency`, capped at 12.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

The 4-core parse is still most of the run. The scoring box has 12 threads; the same one-pass hour buckets are what that machine can overlap. Formatted line count is the other open gap.
