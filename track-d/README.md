# Track D: Traverse

Forward Dijkstra on one CSR. Nodes are renumbered by BFS so a search stays in a small set of 512-node distance pages, which are recycled after every query. The graph is counted, then filled, and the old arrays are released before the queries run.

## Dependencies

None. Static C++20.
