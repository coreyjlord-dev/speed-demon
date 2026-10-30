# Track D: Traverse

Forward Dijkstra on one CSR. A SWAR parse of the mapped file counts the edges, then packs each edge into one word. Threads grow 512-node BFS blobs so a search stays in a few distance pages, recycled after every query. The queue is a Dial bucket of width 8192, with a side heap for the rare huge weights. Build and queries both use every core, capped at 12.

## Dependencies

None. Static C++20.
