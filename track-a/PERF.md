# Track A: performance log

Local numbers are a warm run on this 4-core VM, input already in the page cache. They are not board scores. Board run IDs get filled in when a push is scored. Peak RSS includes the mapped input.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++: fixed-offset hour, open-addressed endpoints, arena latencies, parallel nth_element | run-e7963b19373b | 0.121 | 129 | 56.39 |
| 2 | SWAR endpoint scan, CRC32 hash, drop finished pages, raw output buffer | run-c3130c580f05 | 0.075 | 110 | 61.65 |
| 3 | Two-pass exact uint32 latencies, 1 MB populate/dontneed windows, streamed output | local static | 0.109 | 23.3 | |
| 4 | One pass, per-hour blocks, in-cache select, SIMD field scan, writer thread, THP off | local static | 0.079 | 30.4 | |
| 5 | Tail scan, 64-byte hash, 6-byte records, up to 12 threads, mallopt trim/mmap | local static | 0.065 | 25.0 | |
| 6 | SWAR method end, CRC of three 8-byte samples, multiply latency, release hour text | local static | 0.057 | 23.0 | |
| 7 | In-order write from the hour workers, uninitialized latency buffer, insertion sort through 96 | local static | 0.054 | 22.9 | |
