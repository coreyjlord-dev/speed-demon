# Track B: performance log

Local numbers are a warm run on this 4-core VM. They are not board scores. Peak RSS includes the mapped corpus.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | C++ query-term index, bitsets for wide postings, positional phrases, parallel scan | local | 1.06 | 1,201 | |
