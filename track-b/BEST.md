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
| 7 | 2026-10-30 | | | | Start-bit token walk. Hashes match row 6. scc 211. Single-thread tokenize 158 ms to 113 ms. Local 12-thread wall is within noise on this 4-core VM. |
| 8 | 2026-10-30 | | | 0.436 s | Readable rewrite of row 7. No clang-format guard. scc 1072. Paired 12-thread wall 436 ms vs 445 ms, memory.peak about 78 MB both. Estimated 59.0 vs 64.2. |
| 9 | 2026-10-31 | | | 0.435 s | Leaner readable build. AND/OR/NOT share one list merge; queries share one term reader. scc 1055. Paired 12-thread wall matches row 8. Estimated 59.1. |
| 10 | 2026-10-31 | | | 0.433 s | Phrase anchor matches two bytes before the scalar walk. Paired with row 9 on this box: 12-thread median 455 ms to 433 ms, 8-thread 432 ms to 408 ms. memory.peak unchanged. scc 1072. Estimated 59.6. Dropped: at the board anchor that length scores about 62. |
| 11 | 2026-10-31 | | | 0.343 s | Two-byte phrase anchor, one output write, prefetch group 16, on the board solver. The S=308 figure used a formatter guard and does not count. Guard removed: scc 715, board-scale 0.292 s, est 64.8. Not a win. |

VERIFIED-WIN: 688e93de05b51cc0af2670212ccb015d1b3d0de2 local T=434 RSS=70 src=23925
BOARD-CANDIDATE: c6c556a7e7db00392b4bb1ab35283daed318637e
NOT-A-WIN: guard-free S=715 T=0.292 est=64.8
