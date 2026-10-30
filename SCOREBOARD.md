# Gigatron scoreboard

Board snapshot `2026-10-30T19:58:02Z`. Scored commit `647c13a`. Floors are the best board wall time on each track. A later `speed-demon` push has to beat the floor or that track is reverted.

| Track | Best T | RSS | Points | Time rank | Points rank | Leader points |
|---|---|---|---|---|---|---|
| A | 74.6 ms | 111 MB | 61.72 | 23 of 56 | 28 of 56 | 83.92 Kirk Mates |
| B | 775 ms | 1053 MB | 42.41 | 23 of 46 | 30 of 46 | 66.66 ALL STAR |
| C | 194 ms | 106 MB | 46.49 | 31 of 49 | 34 of 49 | 72.57 ALL STAR |
| D | 860 ms | 663 MB | 42.35 | 27 of 52 | 32 of 52 | 70.19 Cause Of Death |

Best-3 geometric mean of points: **49.56** (A, C, B). D is the track left out.

| Track | Floor run | Floor T | Notes |
|---|---|---|---|
| A | run-f1a16cd3b5e5 | 74.563 ms | `647c13a` re-ran the same binary at 75.5 ms. Floor stands. |
| B | run-2d07601fed36 | 774.512 ms | Same binary as the 777 ms run. `647c13a` was a hair faster, so this is the floor. |
| C | run-d1920a32c8a2 | 193.823 ms | Arena intern. Record on the board is 49.0 ms (ALL STAR). |
| D | run-a333d6934b1f | 860.122 ms | `647c13a` re-ran the same binary at 874 ms. Floor stands. |

`sd-track-a`, `sd-track-b`, and `sd-track-d` were not on origin at this snapshot.
