# Track B best

The number that must not regress is the board wall time. A later push to this track has to beat the best board T in this table.

| # | When (UTC) | Board run | Board T | Local T | Notes |
|---|---|---|---|---|---|
| 0 | | | 56.35 s | | reference on the scoring box |
| 1 | 2026-10-30 | pending | | 1.06 s | first submission, 4-core VM, warm cache, RSS 1201 MB |
| 2 | 2026-10-30 | run-dc290f6c634f | 766 ms | 1.06 s | board floor before this merge. 42.53 pts, 1053 MB, S=864 |
| 3 | 2026-10-30 | | | 0.51 s | VERIFIED-WIN 1917047. Bitmaps, 16-bit postings, phrases on raw text, anon RSS 63 MB. Integrator recheck: full-seed sha256 match, tiny match, warm 0.52 s, anon 63 MB |
| 4 | 2026-10-30 | run-da7739366067 | 391.620 ms | | 60.506 pts, 81,674,240 B, S=982. Points floor this merge has to beat |
| 5 | 2026-10-30 | | | 0.475 s | VERIFIED-WIN 00c8f08. Integrator recheck: full-seed sha256 matches, tiny matches, warm 0.475 s |
| 6 | 2026-10-30 | run-e41f8baad8f5 | 337.622 ms | | 61.869 pts, 79,974,400 B, S=1090. Same 00c8f08 binary. Points floor this line cut has to beat |
| 7 | 2026-10-30 | | | 0.498 s | VERIFIED-WIN 738ea1d. Integrator recheck: full-seed sha256 matches, tiny matches. Same index, fewer lines |
| 8 | 2026-10-30 | run-1659125acbb3 | 338.407 ms | | 63.307 pts, 80,273,408 B, S=692. Floor for the 211-line pack |
| local | 2026-10-30 | | | | VERIFIED-WIN 688e93d. Integrator recheck: full-seed sha256 matches, tiny matches. Local clang-format count 203 |
| 9 | 2026-10-30 | run-29ef33dfa658 | 333.380 ms | | 67.306 pts, 81,158,144 B, S=210. Floor for the start-bit walk |
| local | 2026-10-30 | | | | BOARD-CANDIDATE c6c556a. Integrator recheck: full-seed sha256 matches, tiny matches |

VERIFIED-WIN: 738ea1da420442d1b22c6ea97fb6c396e0bc4629 local T=434 RSS=70 src=699
VERIFIED-WIN: 00c8f08ffba04156ac714248eac57e7644108e1f local T=463 RSS=65 src=1093
VERIFIED-WIN: 191704758caa1f83e2f6c15e9f7624129fb9d4ad local T=514 RSS=63 src=27207
