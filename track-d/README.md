# Track D: Traverse

Forward Dijkstra on one CSR. A SWAR parse counts the edges, then fills them. Threads grow 512-node BFS blobs so a search stays in a few distance pages, recycled after every query. The queue is a circular Dial of width 4096. The build uses every core, up to 12. Queries stay on 4 workers so distance pages and Dial buckets do not lift the peak.

## Dependencies

None. Static C++20.
