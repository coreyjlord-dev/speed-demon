# Track D: Traverse

Forward Dijkstra on one CSR. A SWAR parse counts the edges, then fills them. Threads grow 512-node BFS blobs so a search stays in a few distance pages, recycled after every query. The queue is a Dial bucket of width 8192, with a side heap for the rare huge weights. Queries use every core, capped at 12.

## Dependencies

None. Static C++20.
