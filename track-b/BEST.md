# Track B best

The number that must not regress is the board wall time. A later push to this track has to beat the best board T in this table.

| # | When (UTC) | Board run | Board T | Local T | Notes |
|---|---|---|---|---|---|
| 0 | | | 56.35 s | | reference on the scoring box |
| 1 | 2026-10-30 | pending | | 1.06 s | first submission, 4-core VM, warm cache, RSS 1201 MB |
| 2 | 2026-10-30 | | | 0.51 s | bitmaps, 16-bit postings, phrases on raw text, anon RSS 63 MB |
| 3 | 2026-10-30 | | | 0.46 s | 1 MB 32-bit term table, group prefetch, per-document dedupe. Median anon RSS 65 MB. scc 1093 lines after clang-format (was 985) |
| 4 | 2026-10-30 | | | 0.434 s | Same index and queries. scc 699 lines after clang-format (was 1093). Paired warm cgroup memory.peak 70 MB, same median wall as the previous binary |
| 5 | 2026-10-30 | | | 0.434 s | Plain bitmap OR (cuts on 64-doc boundaries) and a one-multiply term hash. Paired with row 4 on this box: 462 ms down to 434 ms, memory.peak 70 MB, scc 702 |
| 6 | 2026-10-30 | | | 0.434 s | Same binary behavior. Source packed under the formatter limit, scc 211 (was 702). Paired wall matched row 5; warm memory.peak 70 MB |

VERIFIED-WIN: 688e93de05b51cc0af2670212ccb015d1b3d0de2 local T=434 RSS=70 src=23925
