# Track B best

The number that must not regress is the board wall time. A later push to this track has to beat the best board T in this table.

| # | When (UTC) | Board run | Board T | Local T | Notes |
|---|---|---|---|---|---|
| 0 | | | 56.35 s | | reference on the scoring box |
| 1 | 2026-10-30 | pending | | 1.06 s | first submission, 4-core VM, warm cache, RSS 1201 MB |
| 2 | 2026-10-30 | | | 0.51 s | bitmaps, 16-bit postings, phrases on raw text, anon RSS 63 MB |
| 3 | 2026-10-30 | | | 0.46 s | 1 MB 32-bit term table, group prefetch, per-document dedupe. Median anon RSS 65 MB. scc 1093 lines after clang-format (was 985) |

VERIFIED-WIN: 00c8f08ffba04156ac714248eac57e7644108e1f local T=463 RSS=65 src=30276
