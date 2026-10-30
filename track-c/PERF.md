# Track C: performance log

Local numbers are a warm run on this 4-core VM. They are not board scores. Peak RSS includes whatever pages were still mapped.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C++ quote-aware parse, array join, name bitsets, parallel record splits | run-a311de2756a7 | 0.382 | 159 | 37.83 |
| 2 | Buffer name ids per country, then fill one bitset at a time | run-96be9689589e | 0.355 | 152 | 38.69 |
| 3 | Intern names into a per-worker arena, SWAR quotes, one-byte region table, drop finished pages, OR country bitsets in L1 | run-d1920a32c8a2 | 0.194 | 106 | 46.49 |
| 4 | Shard the cross-worker name merge | run-0f3544a1bf60 | 0.156 | 104 | 48.82 |
