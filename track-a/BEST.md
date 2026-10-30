# Track A best

The number that must not regress is the board wall time. A later push has to beat the best board T in this table. Local points use the same K formula; a local point gain is recorded as VERIFIED-WIN before the board re-scores it.

| # | When (UTC) | Board run | Board T | Board RSS | Points | Rank | Notes |
|---|---|---|---|---|---|---|---|
| 0 | | | 32.05 s | 228 MB | 0 | | reference on the scoring box |
| 1 | 2026-10-30 | run-e7963b19373b | 121.3 ms | 129 MB | 56.39 | 32 of 53 | commit a83c542 |
| 2 | 2026-10-30 | run-c3130c580f05 | 75.4 ms | 110 MB | 61.65 | 23 of 53 | commit eca0595. Record 22.7 ms (Kirk Mates, 83.92 pts) |
| 3 | 2026-10-30 | run-f1a16cd3b5e5 | 74.6 ms | 111 MB | 61.72 | 23 of 55 | same A binary, re-scored with b48fa90. Points rank 28 of 55. The 6cd2cb3 re-run was 76.9 ms, so 74.6 ms stays the floor |
| 4 | 2026-10-30 | local | 108.8 ms | 23.3 MB | | | static 4-core warm. Previous local static binary 109.4 ms / 62.5 MB. Same time, RSS 62.5 → 23.3. Local points about 60.1 → 64.4 at S=514 |
| 5 | 2026-10-30 | local | 78.7 ms | 30.4 MB | | | per-hour blocks, SIMD scan, writer thread, THP disabled. Median of 4 warm static runs. Formatted lines about 503. Est. points 66.9 vs local 64.4 and board floor 61.89 |
| 6 | 2026-10-30 | run-01bd0575d976 | 59.9 ms | 33 MB | 69.41 | 23 of 62 | f143946 on the board, commit 07b6200. S=510. Floor this merge has to beat |
| 7 | 2026-10-30 | local | 65.2 ms | 25.0 MB | | | VERIFIED-WIN 3b2062c. Integrator recheck: hash match, warm 72 ms, HWM 24 MB |
| 8 | 2026-10-30 | run-3d4300618475 | 44.885 ms | 30,584,832 B | 72.988 | 15 of 64 | 3b2062c on the board, commit 286ddb4. S=492. Points floor this merge has to beat |
| 9 | 2026-10-30 | local | 65 ms | | | | VERIFIED-WIN 5f407db. Integrator recheck: full-seed sha256 matches, tiny matches, warm 65 ms on this machine (reported 57.1 ms). |
| 10 | 2026-10-30 | run-7f4efd862cbe | 42.031 ms | 31,002,624 B | 74.197 | 13 of 64 | commit 080012c. Points rank 18 of 64. S=415. |
| 11 | 2026-10-30 | run-70324b027658 | 41.141 ms | 32,145,408 B | 74.272 | | same binary, re-scored on a5f4152. S=415. Points floor this merge has to beat. |
| 12 | 2026-10-30 | local | 60 ms | | | | VERIFIED-WIN e66eb72. Integrator recheck: full-seed sha256 matches, tiny matches, warm 60 ms (reported 53.5 ms). |
| 13 | 2026-10-30 | run-f0779c67f52f | 70.601 ms | 32,645,120 B | 68.413 | | commit 6496adb. S=406. Below the 74.272 floor. Reverted to 5f407db. |
| 14 | 2026-10-30 | local | 67 ms | | | | VERIFIED-WIN 59410ea. Integrator recheck: full-seed sha256 matches, tiny matches, warm 67 ms on this machine (reported 58.4 ms / 17.5 MB). |
| 15 | 2026-10-30 | run-e57846948043 | 42.967 ms | 30,912,512 B | 73.787 | | commit 95aaca0. S=439. Below the 74.272 floor. Reverted to 5f407db. |
| local | 2026-10-30 | | 65 ms | | | | VERIFIED-WIN a8d69d8. Integrator recheck: full-seed sha256 matches, tiny matches, warm 65 ms. Same behavior as 5f407db. |
| 16 | 2026-10-30 | run-69f35e2575d7 | 43.757 ms | 33,325,056 B | 74.569 | | commit 89b9471. S=294. Above 74.435. Floor for the 4-byte record push. |

VERIFIED-WIN: 59410ea local T=58.4 RSS=17.5 src=432 lines
VERIFIED-WIN: e66eb72 local T=53.5 RSS=22.9 src=399 lines
VERIFIED-WIN: 5f407db local T=57.1 RSS=23.0 src=408 lines
VERIFIED-WIN: 3b2062c local T=65.2 RSS=25.0 src=13575

VERIFIED-WIN: f143946 local T=78.7 RSS=30.4 src=13589

VERIFIED-WIN: 6bddb8a local T=108.8 RSS=23.3 src=15446
