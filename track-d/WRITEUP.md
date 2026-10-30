# Track D write-up

Queries stop at the target, so one forward search is enough. BFS renumbering puts neighbours in nearby 512-node pages, and those pages are reused instead of a full distance array per thread. Self-loops are dropped. A target with no incoming edge is -1.
