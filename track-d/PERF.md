# Track D: performance log

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference | | 72.69 | 893 | 0 |
| 4 | Scored Dial, both directions | run-a44861ca7ac0 | 0.645 | 291 | 46.01 |
| 5 | One CSR, BFS order, paged distances | local | 0.681 | 85 | ~52 est |
| 6 | SWAR parse, 512-node blobs, radix heap | local | 0.614 | 79 | ~54 est |
| 7 | Dial buckets, same pages and blobs | local | 0.520 | 79 | ~57.5 est |
| 8 | Mapped SWAR parse, one word per edge | local | 0.465 | 65 | ~59.5 est |
| 9 | Readable source, no clang-format off | local | same binary time | 69 | ~60.9 est |
| 10 | Shared settle, heavy join, and checks | local | same binary time | 69 | ~61.1 est |
| 11 | One pool of Dial bucket links | local | ~0.93x | 69 | ~61.7 est |
| 12 | Two-pass CSR, private u16 degrees, no edge buffer | local | 0.469 s vs 0.549 s | 70 | 62.4 est |
| 13 | One scanner, sorted heavy table, no degree fallback | local | tie, about 0.42 s | 60 | 62.9 est |
