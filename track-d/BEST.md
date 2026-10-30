# Track D best

The number that must not regress is the board wall time. A later push has to beat the best board T in this table.

VERIFIED-WIN: 5d27373 local T=681 RSS=85 src=541
VERIFIED-WIN: acb4383 local T=614 RSS=79 src=582
VERIFIED-WIN: aaf13d1 local T=520 RSS=79 src=318

| # | When (UTC) | Board run | Board T | Board RSS | Points | Rank | Notes |
|---|---|---|---|---|---|---|---|
| 0 | | | 72.69 s | 893 MB | 0 | | reference on the scoring box |
| 1 | 2026-10-30 | run-a333d6934b1f | 860 ms | 663 MB | 42.35 | 27 of 52 | commit a83c542. Later re-runs were 864 ms and 874 ms |
| 2 | 2026-10-30 | run-cc0842509e67 | 855 ms | 663 MB | 42.41 | 27 of 54 | same binary, re-scored with ae68be4 |
| 3 | 2026-10-30 | run-bf5059d7be37 | 848 ms | 663 MB | 42.51 | 28 of 55 | commit 4792310, -flto |
| local | 2026-10-30 | | 518 ms | 225 MB | | | VERIFIED-WIN 52f40fd. Integrator recheck matched the full-seed sha256 |
| 4 | 2026-10-30 | run-a44861ca7ac0 | 645 ms | 291 MB | 46.01 | 25 of 55 | commit 532829d. Points rank 29 of 55. S=971. This is the floor. Record 126 ms (Cause Of Death, 71.7 pts) |
| local | 2026-10-30 | | 681 ms | 85 MiB | ~52 est | | VERIFIED-WIN 5d27373. One CSR, BFS order, paged distances. src is approximate formatted code lines. |
| local | 2026-10-30 | | 614 ms | 79 MiB | ~54 est | | VERIFIED-WIN acb4383. SWAR parse, 512-node blobs, radix heap. Static binary, full seed 1. |
| local | 2026-10-30 | | 520 ms | 79 MiB | ~57 est | | VERIFIED-WIN aaf13d1. Dial buckets. Static full seed 1, four runs 506–552 ms, median 520. src is folder lines after clang-format. |
