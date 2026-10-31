# Track D best

The number that must not regress is the board wall time. A later push has to beat the best board T in this table.

VERIFIED-WIN: 5d27373 local T=681 RSS=85 src=541
VERIFIED-WIN: 42c54bd local T=508 RSS=80 src=318
VERIFIED-WIN: 6eed07d local T=465 RSS=65 src=317
READABLE-WIN: ebda02e formatted S=645 est 60.9 points (board 328.9 ms, 69 MB held)
READABLE-WIN: e99118c formatted S=610 est 61.1 points (board 328.9 ms, 69 MB held)
READABLE-WIN: 30f5ece S=624 est=61.73
HONEST-S: 6eed07d guarded scc 278 is invalid. Formatter-stable S=603. At 331 ms and 69 MB that is 61.06, not 63.57.
VERIFIED-WIN: aaf13d1 local T=520 RSS=79 src=318
VERIFIED-WIN: acb4383 local T=614 RSS=79 src=582

| # | When (UTC) | Board run | Board T | Board RSS | Points | Rank | Notes |
|---|---|---|---|---|---|---|---|
| 0 | | | 72.69 s | 893 MB | 0 | | reference on the scoring box |
| 1 | 2026-10-30 | run-a333d6934b1f | 860 ms | 663 MB | 42.35 | 27 of 52 | commit a83c542. Later re-runs were 864 ms and 874 ms |
| 2 | 2026-10-30 | run-cc0842509e67 | 855 ms | 663 MB | 42.41 | 27 of 54 | same binary, re-scored with ae68be4 |
| 3 | 2026-10-30 | run-bf5059d7be37 | 848 ms | 663 MB | 42.51 | 28 of 55 | commit 4792310, -flto |
| local | 2026-10-30 | | 518 ms | 225 MB | | | VERIFIED-WIN 52f40fd. Integrator recheck matched the full-seed sha256 |
| 4 | 2026-10-30 | run-a44861ca7ac0 | 645 ms | 291 MB | 46.01 | 25 of 55 | commit 532829d. Points rank 29 of 55. S=971. This is the floor. Record 126 ms (Cause Of Death, 71.7 pts) |
| 5 | 2026-10-30 | run-575db424045a | 638 ms | 288 MB | 46.17 | 25 of 55 | same Dial binary, re-scored. Current time floor |
| local | 2026-10-30 | | 681 ms | 85 MiB | ~52 est | | VERIFIED-WIN 5d27373. Integrator recheck: hash match, warm 0.68 s / 85 MB. Slower than local Dial 0.56 s, much smaller |
| 6 | 2026-10-30 | run-310c50829b3f | 697 ms | 89 MB | 52.37 | 26 of 56 | commit 286ddb4, paged distances. Points rank 25 of 56. S=518. |
| 7 | 2026-10-30 | run-93fd12fea066 | 678 ms | 90,124,288 B | 52.609 | 25 of 57 | same paged binary, re-scored on 9710a7a. Points best before acb4383. |
| local | 2026-10-30 | | 587 ms | | | | VERIFIED-WIN acb4383. Integrator recheck: full-seed sha256 a23dbdf0 matches, tiny matches, warm 0.587 s. Replaces 5d27373. |
| 8 | 2026-10-30 | run-b062dc46fc08 | 413.966 ms | 85,639,168 B | 57.993 | 22 of 57 | commit 080012c. Points rank 19 of 57. S=550. |
| 9 | 2026-10-30 | run-0c8ab4d70dac | 403.530 ms | 85,532,672 B | 58.275 | | same binary, re-scored on a5f4152. S=550. Points floor this merge has to beat. |
| local | 2026-10-30 | | 524 ms | | | | VERIFIED-WIN aaf13d1. Integrator recheck: full-seed sha256 matches, tiny matches, warm 0.524 s (reported 520 ms). |
| 10 | 2026-10-30 | run-f7d5e71800b6 | 362.082 ms | 89,636,864 B | 61.345 | 17 of 57 | commit 6496adb. Points rank 8 of 57. S=289. |
| 11 | 2026-10-30 | run-23b521c3b8dc | 355.748 ms | 89,526,272 B | 61.542 | | same binary, re-scored on d3853f9. S=289. Points floor this merge has to beat. |
| local | 2026-10-30 | | 562 ms | | | | VERIFIED-WIN 42c54bd. Integrator recheck: full-seed sha256 matches, tiny matches. This 4-core VM does not show the 12-thread query cap. |
| 12 | 2026-10-30 | run-862c09bafae5 | 526.757 ms | 85,336,064 B | 57.488 | | commit e48c45c. S=289. Below the 61.542 floor. Reverted to aaf13d1. |
| local | 2026-10-30 | | 497 ms | | | | VERIFIED-WIN 6eed07d. Integrator recheck: full-seed sha256 matches, tiny matches, warm 0.497 s on 4 cores (reported 0.465 s / 65 MiB at 12 threads). |
| 13 | 2026-10-30 | run-177ddcf318b6 | 333.666 ms | 69,804,032 B | 63.363 | 15 of 57 | commit 069146c. Points rank 6 of 57. S=285. This is the floor. |
| local | 2026-10-30 | | 329 ms | 69 MB | ~60.9 est | | Readable form of 6eed07d. clang-format LLVM, no off region, lines at most 80. scc S=645. Same hashes. Warm 4-thread time and cgroup peak match 6eed07d. |
| local | 2026-10-31 | | 329 ms | 69 MB | ~61.1 est | | Leaner readable form, commit e99118c. scc S=610. Same hashes. Warm 4-thread median 0.530 s vs 0.539 s for ebda02e; cgroup peak 59.5–59.9 MiB either way. |
| local | 2026-10-31 | | 308 ms est | 69 MB | 61.73 est | | Pooled Dial, commit 30f5ece. scc S=624. Same hashes. Warm median 0.478 s vs 0.514 s at 4 threads, 1.451 s vs 1.556 s at 1 thread. Cgroup peak 59.8 MiB either way. Board time scaled from the 331 ms floor. |
