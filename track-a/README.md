# Track A: Ingest

Group 3.1M request lines by UTC hour and endpoint, then emit count, nearest-rank p50/p95/p99, 5xx count, and floor error ppm.

## Approach

The timestamp is a fixed 27-byte field inside one 72-hour window, so the hour is a few digits at fixed offsets, not a calendar parse. The newline scan starts at byte 39, the shortest legal line, with a 64-byte compare and a prefetch of the next chunk. One pass files a 4-byte record into that hour: an 11-bit endpoint id, an error bit, an overflow bit, and a 19-bit latency. Larger latencies sit in a per-hour side list. The method ends at a branchless SWAR space, the latency is a masked 8-digit horner, and the endpoint hash is a CRC of the first, middle, and last 8 bytes; length and hash are compared before any `memcmp`. Each hour is counting-sorted by endpoint. Groups of up to 64 values are insertion-sorted; larger groups are radix-sorted, and the three nearest ranks are read from that order. Records sit in blocks of 8192. A writer thread emits hours in order and releases each buffer after the write. The run uses every hardware thread up to 12. Transparent huge pages are off, and `mallopt` (`M_TRIM_THRESHOLD`, `M_MMAP_THRESHOLD`) returns freed blocks to the OS. Finished pages of the mapping are dropped; the file is not populated up front.

Status is an error exactly when the first digit is `5` (500–599). Duplicate lines both count. Workers equal one per physical core when the run exposes hyperthreads.

## Dependencies

None. Static C++20, libstdc++ and libc linked in.

## With another 24 hours

The 4-core parse is still most of the run. The scoring box has 12 threads; the same one-pass hour buckets are what that machine can overlap. Formatted line count is the other open gap.
