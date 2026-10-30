# Gigatron scoreboard

Board snapshot `2026-10-30T20:06:39Z`. Scored commit `f8e67ee`. Floors are the best board wall time on each track. A later `speed-demon` push has to beat the floor or that track is reverted.

| Track | Best T | RSS | Points | Time rank | Points rank | Leader points |
|---|---|---|---|---|---|---|
| A | 74.6 ms | 111 MB | 61.72 | 24 of 57 | 28 of 57 | 83.92 Kirk Mates |
| B | 766 ms | 1053 MB | 42.53 | 23 of 47 | 30 of 47 | 66.66 ALL STAR |
| C | 183 ms | 106 MB | 47.11 | 32 of 51 | 35 of 51 | 72.57 ALL STAR |
| D | 860 ms | 663 MB | 42.35 | 27 of 53 | 32 of 53 | 70.19 Cause Of Death |

Best-3 geometric mean of points: **49.82** (A, C, B). D is the track left out.

| Track | Floor run | Floor T | Notes |
|---|---|---|---|
| A | run-f1a16cd3b5e5 | 74.563 ms | `f8e67ee` re-ran the same binary at 75.3 ms. Floor stands. |
| B | run-dc290f6c634f | 766.419 ms | Same binary. `f8e67ee` replaced the 775 ms floor. |
| C | run-c3cd8ca0883c | 183.058 ms | Same arena-intern binary. `f8e67ee` replaced the 194 ms floor. Record is 49.0 ms (ALL STAR). |
| D | run-a333d6934b1f | 860.122 ms | `f8e67ee` re-ran the same binary at 865 ms. Floor stands. |

`sd-track-a`, `sd-track-b`, and `sd-track-d` were not on origin at this snapshot. Next `speed-demon` push carries a sharded name-merge for C (local warm 137 ms, full-seed sha256 match).
