# Track D write-up

One forward radix-heap search per query, stopped at the target. Parallel BFS blobs of 512 nodes keep that search in a handful of pages, wiped and reused instead of a full distance array per thread. Self-loops are dropped. A target with no incoming edge is -1.
