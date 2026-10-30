# Gigatron scoreboard

Board snapshot `2026-10-30T21:50:09Z`, head `6496adb`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

D is 1 on every track. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. The mapped input is prewarmed outside that cgroup.

| Track | Commit | Run | T | RSS | S | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | `a5f4152` | run-70324b027658 | 41.141 ms | 32,145,408 | 415 | 74.272 | 13 of 64 | 18 of 64 |
| B | `6496adb` | run-fa4b9f4d7267 | 338.400 ms | 80,154,624 | 1090 | 61.834 | 8 of 51 | 9 of 51 |
| C | `9710a7a` | run-d2451a1db7a8 | 74.864 ms | 35,385,344 | 582 | 61.813 | 19 of 55 | 24 of 55 |
| D | `6496adb` | run-f7d5e71800b6 | 362.082 ms | 89,636,864 | 289 | 61.345 | 17 of 57 | 8 of 57 |

Best-3 geometric mean of points: **65.68** (A, B, D).

| Track | Leader points | Leader | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|
| A | 83.961 | Kirk Mates | 22.77 ms | 20.5 MB | 260 |
| B | 67.211 | Cause Of Death | 283.77 ms | 41.1 MB | 837 |
| C | 74.258 | Cause Of Death | 36.77 ms |  | 563 |
| D | 72.344 | Cause Of Death | 126.58 ms |  | 447 |

Reference anchors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Points floors that must not drop: A 74.272 (`a5f4152`, 41.141 ms, S=415), B 61.834 (`6496adb`, 338.400 ms, S=1090), C 61.813 (`9710a7a`), D 61.345 (`6496adb`, 362.082 ms, S=289). `e66eb72` scored 70.601 ms and 68.413 points, so Track A is reverted to `5f407db`. B `00c8f08` and D `aaf13d1` stay.
