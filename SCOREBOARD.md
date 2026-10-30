# Gigatron scoreboard

Board snapshot `2026-10-30T20:44:32Z`, commit `532829d`. Machine points are `50 * log10(K)` with

`K = (T_ref/T)^0.50 * min(M_ref/M, 50)^0.20 * min(S_ref/S, 1)^0.15 * (D_ref/D)^0.15`.

S and D are capped at 1, so they can only cost points. D_ref is 1. Ours is already 1: no manifest dependencies, and `.speed-demon.toml` declares `dependency_count = 0`. The static libstdc++ pulled in by `-static` is not a dependency. Comments, blank lines, Markdown, and TOML are not source. S is scc code lines after `clang-format --style=LLVM`. M is cgroup `memory.peak`. `/data` is prewarmed outside that cgroup, so the mapped input is not the RSS we are scored on. Anonymous tables are.

| Track | T factor | M factor | S factor | D factor | K | Points | Time rank | Points rank |
|---|---|---|---|---|---|---|---|---|
| A | 20.86 | 1.16 | 0.72 | 1.00 | 17.24 | 61.89 | 26 of 62 | 31 of 62 |
| B | 8.58 | 1.16 | 0.71 | 1.00 | 7.09 | 42.53 | 23 of 48 | 31 of 48 |
| C | 9.62 | 1.29 | 0.76 | 1.00 | 9.47 | 48.84 | 31 of 53 | 34 of 53 |
| D | 10.61 | 1.25 | 0.63 | 1.00 | 8.29 | 46.01 | 25 of 55 | 29 of 55 |

Best-3 geometric mean of points: **51.81** (A, C, D).

| Track | Our T | Our RSS | Our S | Leader points | Leader T | Leader RSS | Leader S |
|---|---|---|---|---|---|---|---|
| A | 73.6 ms | 110 MB | 514 | 83.96 Kirk Mates | 22.7 ms | 22 MB | 260 |
| B | 766 ms | 1053 MB | 864 | 66.85 Kirk Mates | 314 ms | 39 MB | 670 |
| C | 156.0 ms | 103 MB | 648 | 73.65 Cosmic | 51.1 ms | 10 MB | 294 |
| D | 645 ms | 291 MB | 971 | 71.69 Cause Of Death | 156 ms | 82 MB | 450 |

Reference anchors used for the factors: A 32.047 s / 228 MB / S 56, B 56.35 s / 2200 MB / S 91, C 14.44 s / 367 MB / S 108, D 72.69 s / 893 MB / S 43.

Where the points leak: time on every track, then source size. D's Dial merge cut time and RSS, and S jumped from 382 to 971, so D's S factor fell to 0.63. B's RSS is still the largest memory leak.

Time floors that must not regress: A 73.624 ms (`532829d`), B 766.419 ms, C 156.004 ms, D 645.367 ms (`532829d`, Dial). D's source grew to 971 lines; that S factor is now the main D leak.
