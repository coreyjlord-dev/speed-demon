# Track D: Traverse

Shortest paths on a directed road graph: 1.74M nodes, 5M edges, 2,000 queries. The whole process is timed, including the read.

## Approach

Edges are parsed into a CSR. Parallel edges keep the minimum weight and self-loops are dropped; neither can improve a shortest path. Each query is a bidirectional Dijkstra on a 4-bucket radix heap, forward on the graph and backward on the reversed graph. The side that has settled fewer nodes expands next, so an island or a dead end finishes before the search walks the main component. The search stops when the sum of the two heap minima is at least the best s-t walk already found, or when either frontier empties. `s == t` is 0. A source with no outgoing edge, or a target with no incoming edge, is -1 with no search.

Labels live in epoch-stamped arrays, one set per worker, so a query does not clear 1.7M distances. Queries are stolen from a shared counter.

## Dependencies

None. Static C++20.

## With another 24 hours

Contraction hierarchies or a few ALT landmarks. Two thousand queries is small enough that a fast preprocess plus microsecond queries can beat a well tuned bidirectional search. The record is 203 ms.
