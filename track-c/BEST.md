# Track C best

The number that must not regress is the board wall time. A later push has to beat the best board T in this table.

| # | When (UTC) | Board run | Board T | Board RSS | Points | Rank | Notes |
|---|---|---|---|---|---|---|---|
| 0 | | | 14.44 s | 367 MB | 0 | | reference on the scoring box |
| 1 | 2026-10-30 | run-33950e9306be | 395 ms | 160 MB | 37.45 | 32 of 45 | commit a83c542 |
| 2 | 2026-10-30 | run-a311de2756a7 | 382 ms | 159 MB | 37.83 | 30 of 45 | same binary, re-scored with eca0595 |
| 3 | 2026-10-30 | run-96be9689589e | 355 ms | 152 MB | 38.69 | 33 of 49 | commit 6cd2cb3. Points rank 35 of 49 |
| 4 | 2026-10-30 | run-d1920a32c8a2 | 194 ms | 106 MB | 46.49 | 31 of 49 | commit 647c13a |
| 5 | 2026-10-30 | run-c3cd8ca0883c | 183 ms | 106 MB | 47.11 | 32 of 51 | same binary, re-scored with f8e67ee. Points rank 35 of 51 |
| local | 2026-10-30 | | 137 ms | 66 MB | | | verified local win before the push. Byte-exact full-seed sha256. |
| 6 | 2026-10-30 | run-0f3544a1bf60 | 156.2 ms | 104 MB | 48.82 | 31 of 51 | commit ae68be4. S=648 |
| 7 | 2026-10-30 | run-945982b45535 | 156.0 ms | 103 MB | 48.84 | 31 of 53 | commit 4792310, -flto. S=648. |
| 8 | 2026-10-30 | run-4bbd45a50a0d | 151.251 ms | 103,759,872 B | 49.155 | 31 of 53 | commit 07b6200, same binary re-score. Points rank 35 of 53. S=648. This is the floor. |
| local | 2026-10-30 | | 82 ms | anon ~30 MB | | | one-pass memchr parse, shared name table, per-country L1 bitset. Full-seed sha256 d8088b1c matches. Tiny and worked match. Not scored yet. |
