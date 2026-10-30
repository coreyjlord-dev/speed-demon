# Track D write-up

One forward search per query, stopped at the target. Edge weights under 8192 sit in a Dial bucket; the few huge weights use a side heap. Parallel BFS blobs of 512 nodes keep the search in a handful of pages, wiped and reused instead of a full distance array per thread. Self-loops are dropped. A target with no incoming edge is -1.
