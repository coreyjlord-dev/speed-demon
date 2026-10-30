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

VERIFIED-WIN: 6bddb8a local T=108.8 RSS=23.3 src=15446
