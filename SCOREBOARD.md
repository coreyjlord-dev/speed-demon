# Gigatron scoreboard

Board snapshot `2026-10-30T21:09:53Z`, head `286ddb4`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

D is 1 on every track. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. The mapped input is prewarmed outside that cgroup.

| Track | Commit | Run | T | RSS | S | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | `286ddb4` | run-3d4300618475 | 44.885 ms | 30,584,832 | 492 | 72.988 | 13 of 62 | 16 of 62 |
| B | `40917bf` | run-9d1f6eed4934 | 392.343 ms | 82,182,144 | 982 | 60.460 | 11 of 49 | 10 of 49 |
| C | `07b6200` | run-4bbd45a50a0d | 151.251 ms | 103,759,872 | 648 | 49.155 | 31 of 53 | 35 of 53 |
| D | `286ddb4` | run-310c50829b3f | 696.632 ms | 89,059,328 | 518 | 52.367 | 26 of 56 | 25 of 56 |

Best-3 geometric mean of points: **61.36** (A, B, D).

| Track | Leader points | Leader | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|
| A | 83.961 | Kirk Mates | 22.77 ms | 20.5 MB | 260 |
| B | 66.849 | Kirk Mates | 313.65 ms | 37.5 MB | 670 |
| C | 73.650 | Cosmic | 51.06 ms | 9.6 MB | 294 |
| D | 71.790 | Cause Of Death | 126.38 ms | 80.1 MB | 426 |

Reference anchors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Time floors that must not regress: A 44.885 ms (`286ddb4`), B 392.343 ms (`40917bf`), C 151.251 ms (`07b6200`), D 638.493 ms was the previous time floor. D's paged run is slower (696.632 ms) and higher points (52.367 vs 46.167), and the board kept it as the points best. The next C push has to beat 151.251 ms.
