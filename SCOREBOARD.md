# Gigatron scoreboard

Board snapshot `2026-10-30T21:50:09Z`, head `6496adb`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

D is 1 on every track. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. The mapped input is prewarmed outside that cgroup.

| Track | Commit | Run | T | RSS | S | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | `e48c45c` | run-4c86463b810f | 41.140 ms | 30,961,664 | 415 | 74.435 | 14 of 64 | 18 of 64 |
| B | `6496adb` | run-fa4b9f4d7267 | 338.400 ms | 80,154,624 | 1090 | 61.834 | 8 of 51 | 9 of 51 |
| C | `9710a7a` | run-d2451a1db7a8 | 74.864 ms | 35,385,344 | 582 | 61.813 | 19 of 55 | 24 of 55 |
| D | `d3853f9` | run-23b521c3b8dc | 355.748 ms | 89,526,272 | 289 | 61.542 | 16 of 57 | 9 of 57 |

Best-3 geometric mean of points: **65.68** (A, B, D).

| Track | Leader points | Leader | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|
| A | 83.961 | Kirk Mates | 22.77 ms | 20.5 MB | 260 |
| B | 67.211 | Cause Of Death | 283.77 ms | 41.1 MB | 837 |
| C | 74.258 | Cause Of Death | 36.77 ms |  | 563 |
| D | 72.344 | Cause Of Death | 126.58 ms |  | 447 |

Reference anchors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Points floors that must not drop: A 74.435 (`e48c45c`, 41.140 ms, S=415), B 61.834 (`6496adb`, 338.400 ms, S=1090), C 61.813 (`9710a7a`), D 61.542 (`d3853f9`, 355.748 ms, S=289). This push replaces D with `6eed07d`. Revert D if points fall below 61.542.
