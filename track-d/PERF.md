# Track D: performance log

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference | | 72.69 | 893 | 0 |
| 4 | Scored Dial, both directions | run-a44861ca7ac0 | 0.645 | 291 | 46.01 |
| 5 | One CSR, BFS order, paged distances | local | 0.681 | 85 | ~52 est |
| 6 | SWAR parse, 512-node blobs, radix heap | local | 0.614 | 79 | ~54 est |
| 7 | Dial buckets, same pages and blobs | local | 0.520 | 79 | ~57.5 est |
| 8 | Mapped SWAR parse, one word per edge | local | 0.465 | 65 | ~59.5 est |
| 9 | Readable source, no clang-format off | local | same binary time | 69 | ~60.9 est |
| 10 | Shared settle, heavy join, and checks | local | same binary time | 69 | ~61.1 est |
| 11 | One pool of Dial bucket links | local | ~0.93x | 69 | ~61.7 est |
| 12 | Two-pass CSR, private u16 degrees, no edge buffer | local | 0.469 s vs 0.549 s | 70 | 62.4 est |
| 13 | One scanner, sorted heavy table, no degree fallback | local | tie, about 0.42 s | 60 | 62.9 est |
| 14 | Board read of speed-demon `1c48b21` (two-pass, not `4397e52`) | run-998aaaf7ccc9 | 0.385 s | 72.5 | 58.92 |
| 14b | Kept board best, guarded `6eed07d` | run-941d46f514cc | 0.329 s | 69.1 | 63.566 |
| 15 | Board read, Cause Of Death leads Track D | run-fd84bc908d3e | 0.122 s | 67.6 | 73.032 |
| 15b | Latest speed-demon two-pass | run-cd06bca6b110 | 0.380 s | 71.9 | 59.099 |

Time experiments on the 4-core VM, full graph, `SD_THREADS=4`, medians. Baseline about 0.44–0.48 s. Skipping the blob renumber and querying original ids: 0.63 s. Degree-sorted ids: 0.77 s. Bidirectional Dijkstra with `top_f + top_b >= best`: hashes matched, 0.51 s. The reverse ball settled 10.7e6 nodes and the forward ball 3.6e6, against about 8e6 one-way pops, and building the reverse CSR took 0.03 s. One exhaustive single-source search is 0.07–0.09 s, so a few ALT landmarks do not pay back the 0.205 s of queries. Degree counts cannot be fused into the scatter without storing the edges first. Queries need the finished CSR, so a thread that finishes its parse slice cannot start them early.

Query profile, same machine, per-thread counters summed. 1988 searched queries, 23,227,082 settles (11,684 per query), 44.5 distance chunks, 1,460 dial buckets, 13,300 pool links. Thread time: reset 0.2 ms, dial clear 1.5 ms, chunk memset 7.6 ms, search loop 828 ms. Per query that is 0.1 µs + 0.8 µs + 3.8 µs + 416 µs. A hot repeat of one query is 325 µs. Epoch stamps in the distance word, with no chunk memset, matched all seven hashes and the batch median went from 0.417 s to 0.429 s.

Load floor: `graph.txt` is 91,950,247 bytes, n=1,740,512, m=5,000,000. A warm mmap word-sum is 2.6 ms (35 GB/s); the first pass is 13 ms. Count 32 ms and fill 62 ms are already private `u16` degrees, one prefix, and a parallel scatter. Blobs 70 ms and remap 36 ms do not read the file.

Blobs and remap are both parallel. On this 4-core VM, blobs 0.114 / 0.116 / 0.070 / 0.059 / 0.053 s at 1, 2, 4, 8, 12 threads; remap 0.105 / 0.061 / 0.034 / 0.035 / 0.034 s. Remap stops scaling at 4 cores. Blobs scale poorly (random atomics). `perf` is not installed. First-neighbor radix reorder (shifts 8, 10, 12) matched hashes and landed at 0.64–0.69 s. Weights: 4,875,003 below 256, 124,439 in [256, 65536), 50 at or above 65536, max 2^32-1, so neither u8 nor u16 holds every weight. The packed u32 edge is already 4 bytes. Prefetch of the neighbor's distance line and edge row: median 0.412 s vs 0.428 s, S=568.

Balanced bidirectional search, expand the side with fewer settled nodes, stop when `top_f + top_b >= best` or either side is empty. Reverse CSR built after the remap. Full-graph median 0.327 s vs 0.412 s. Settles 3,581,678 forward and 3,580,687 backward, 3,581 per query, against 11,684 one-way. Cgroup peak 66 MiB vs 60 MiB. S=672. All seven hashes match.

Count+fill on this 4-core VM: 0.340 s at 1 thread, 0.182 s at 4, 0.119 s at 12 (oversubscribed). Blobs at those widths: 0.113 / 0.130 / 0.072 s. The 12-thread board will not see count+fill under 0.06 s from this parse.

Rejected on the 4-core VM, hashes matched unless noted. Shared atomic degrees (the shape inside `6eed07d`): 0.66 s vs 0.49 s. Full-component BFS instead of 512-node blobs: 0.53 s vs 0.51 s. Goal pruning plus static query slices: 0.53 s vs 0.50 s. Dial 4096/2048/1024 tied; Dial 512 changed the full hash because a far entry can fall back into the dial window. Empty-bucket steps were 1.8e6 against 8.7e6 relaxes, and the far heap fired 20 times, so dial width is not the query cost. 1979 of 2000 queries have a unique source. A `u32` weight per edge would add about 20 MB and lose more points than the lines it deletes.
