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
| 6 | 2026-10-30 | local | 65.2 ms | 25.0 MB | | | Tail scan, masked 64-byte hash, 6-byte records, mallopt trim/mmap, up to 12 threads. Median of 8 warm static runs on 4 cores. Formatted lines about 485. Est. points 69.9 |
| 7 | 2026-10-30 | local | 57.1 ms | 23.0 MB | | | SWAR method end, CRC of three 8-byte samples, multiply latency, hour text released after write. Median of 8 warm static runs on 4 cores. Formatted lines 408. Est. points 72.2 |
| 8 | 2026-10-30 | local | 53.5 ms | 22.9 MB | | | Hour workers write in order, latency buffer is not zeroed, insertion sort through 96. Median of 8 warm static runs on 4 cores. Formatted lines 399. Est. points 73.0. Board rejected this line: 70.6 ms / 68.41 pts |
| 9 | 2026-10-30 | local | 52.2 ms | 22.9 MB | | | Radix sort for groups of 512 and up. Built on the reverted line, so it is not the floor |
| 10 | 2026-10-30 | board | 41.1 ms | | 74.27 | | 5f407db restored by the integrator after the points drop. Local reference remains 57.1 ms / 23.0 MB / 408 lines / 72.22 pts |
| 11 | 2026-10-30 | local | 58.4 ms | 17.5 MB | | | 4-byte packed records, overflow list, malloc arenas capped at 2. Paired with 5f407db in the same session (61.1 ms / 22.7 MB). 12 threads 65.7 ms / 22.2 MB vs 5f407db 68.0 ms / 28.4 MB. Formatted lines 432. Est. points 72.98 |

VERIFIED-WIN: 59410ea local T=58.4 RSS=17.5 src=12273

VERIFIED-WIN: 5f407db local T=57.1 RSS=23.0 src=11635

VERIFIED-WIN: 3b2062c local T=65.2 RSS=25.0 src=13575

VERIFIED-WIN: f143946 local T=78.7 RSS=30.4 src=13589

VERIFIED-WIN: 6bddb8a local T=108.8 RSS=23.3 src=15446
