# Track C: performance log

Local numbers are a warm run on this 4-core VM. They are not board scores. Peak RSS includes the mapped CSV.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C++ quote-aware parse, array join, name bitsets, parallel record splits | run-a311de2756a7 | 0.382 | 159 | 37.83 |
| 2 | Buffer name ids per country, then fill one L1-resident bitset at a time | local, pending board | 0.264 | 264 | |
