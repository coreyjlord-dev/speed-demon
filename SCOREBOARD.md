# Gigatron scoreboard

Board snapshot `2026-10-30T21:23:42Z`, head `9710a7a`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

D is 1 on every track. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. The mapped input is prewarmed outside that cgroup.

| Track | Commit | Run | T | RSS | S | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | `286ddb4` | run-3d4300618475 | 44.885 ms | 30,584,832 | 492 | 72.988 | 15 of 64 | 17 of 64 |
| B | `40917bf` | run-9d1f6eed4934 | 392.343 ms | 82,182,144 | 982 | 60.460 | 11 of 51 | 10 of 51 |
| C | `9710a7a` | run-d2451a1db7a8 | 74.864 ms | 35,385,344 | 582 | 61.813 | 19 of 55 | 24 of 55 |
| D | `9710a7a` | run-93fd12fea066 | 678.055 ms | 90,124,288 | 518 | 52.609 | 25 of 57 | 25 of 57 |

Best-3 geometric mean of points: **64.85** (A, C, B).

| Track | Leader points | Leader | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|
| A | 83.961 | Kirk Mates | 22.77 ms | 20.5 MB | 260 |
| B | 66.849 | Kirk Mates | 313.65 ms | 37.5 MB | 670 |
| C | 73.650 | Cosmic | 51.06 ms | 9.6 MB | 294 |
| D | 71.790 | Cause Of Death | 126.38 ms | 80.1 MB | 426 |

Reference anchors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Time floors that must not regress: A 44.885 ms (`286ddb4`), B 392.343 ms (`40917bf`), C 74.864 ms (`9710a7a`), D 638.493 ms was the Dial time. D's paged solver is the points best at 678.055 ms and 52.609 points (was 46.167 at 638 ms). A re-run of `9710a7a` left A's and B's bests in place.
