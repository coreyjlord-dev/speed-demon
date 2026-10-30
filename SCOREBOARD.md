# Gigatron scoreboard

Board snapshot `2026-10-30T21:34:37Z`, head `080012c`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

D is 1 on every track. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. The mapped input is prewarmed outside that cgroup.

| Track | Commit | Run | T | RSS | S | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | `080012c` | run-7f4efd862cbe | 42.031 ms | 31,002,624 | 415 | 74.197 | 13 of 64 | 18 of 64 |
| B | `25fd2dd` | run-da7739366067 | 391.620 ms | 81,674,240 | 982 | 60.506 | 11 of 51 | 10 of 51 |
| C | `9710a7a` | run-d2451a1db7a8 | 74.864 ms | 35,385,344 | 582 | 61.813 | 19 of 55 | 24 of 55 |
| D | `080012c` | run-b062dc46fc08 | 413.966 ms | 85,639,168 | 550 | 57.993 | 22 of 57 | 19 of 57 |

Best-3 geometric mean of points: **65.23** (A, C, B).

| Track | Leader points | Leader | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|
| A | 83.961 | Kirk Mates | 22.77 ms | 20.5 MB | 260 |
| B | 67.211 | Cause Of Death | 283.77 ms | 41.1 MB | 837 |
| C | 73.907 | Cosmic | 49.69 ms | 9.9 MB | 284 |
| D | 72.326 | Cause Of Death | 128.01 ms | 66.2 MB | 447 |

Reference anchors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Points floors that must not drop: A 74.197 (`080012c`, 42.031 ms, S=415), B 60.506 (`25fd2dd`), C 61.813 (`9710a7a`), D 57.993 (`080012c`, 413.966 ms, S=550). A `5f407db` and D `acb4383` both cleared the previous floors (72.988 and 52.609).
