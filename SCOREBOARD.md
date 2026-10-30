# Gigatron scoreboard

Board snapshot `2026-10-30T20:11:00Z`. Scored commit `ae68be4`. Floors below are the best board wall time on each track. A later `speed-demon` push has to beat that time or that track is reverted.

| Track | Best T | RSS | Points | Time rank | Points rank | Leader points |
|---|---|---|---|---|---|---|
| A | 74.6 ms | 111 MB | 61.72 | 24 of 59 | 29 of 59 | 83.92 Kirk Mates |
| B | 766 ms | 1053 MB | 42.53 | 23 of 47 | 30 of 47 | 66.78 Kirk Mates |
| C | 156 ms | 104 MB | 48.82 | 31 of 51 | 34 of 51 | 72.57 ALL STAR |
| D | 855 ms | 663 MB | 42.41 | 27 of 54 | 33 of 54 | 70.19 Cause Of Death |

Best-3 geometric mean of points: **50.42** (A, C, B). D is the track left out.

The board keeps the run with the most points. A's points-best is `ae68be4` at 74.8 ms and 61.75 points (RSS 110 MB). The fastest A time is still 74.6 ms (`run-f1a16cd3b5e5`), and that remains the time floor.

| Track | Floor run | Floor T | Notes |
|---|---|---|---|
| A | run-f1a16cd3b5e5 | 74.563 ms | Same binary re-ran at 74.8 ms with slightly more points. Not a code change. |
| B | run-dc290f6c634f | 766.419 ms | `ae68be4` re-ran the same binary at 802 ms. Floor stands. |
| C | run-0f3544a1bf60 | 156.166 ms | Sharded name merge, `ae68be4`. Down from 183 ms. Record is 49.0 ms (ALL STAR). |
| D | run-cc0842509e67 | 854.988 ms | Same binary, a faster re-run of the 860 ms floor. |

`sd-track-a`, `sd-track-b`, and `sd-track-d` were not on origin at this snapshot. `sd-track-c` is the C working branch.
