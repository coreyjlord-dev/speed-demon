# Track A: performance log

Local numbers are a warm run on this 4-core VM, input already in the page cache. They are not board scores. Board run IDs get filled in when a push is scored. Peak RSS includes the mapped input.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++: fixed-offset hour, open-addressed endpoints, arena latencies, parallel nth_element | run-e7963b19373b | 0.121 | 129 | 56.39 |
| 2 | SWAR endpoint scan, CRC32 hash, drop finished pages, raw output buffer | run-c3130c580f05 | 0.075 | 110 | 61.65 |
| 3 | Two-pass exact uint32 latencies, 1 MB populate/dontneed windows, streamed output | local static | 0.109 | 23.3 | |
