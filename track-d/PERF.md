# Track D: performance log

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference | | 72.69 | 893 | 0 |
| 4 | Scored Dial, both directions | run-a44861ca7ac0 | 0.645 | 291 | 46.01 |
| 5 | One CSR, BFS order, paged distances | local | 0.681 | 85 | ~52 est |
| 6 | SWAR parse, 512-node blobs, radix heap | local | 0.614 | 79 | ~54 est |
| 7 | Dial buckets, same pages and blobs | local | 0.520 | 79 | ~58 est |
