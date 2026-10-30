# Track D: Traverse

Bidirectional Dial on a CSR whose nodes are renumbered in BFS order. The graph is parsed in parallel and built with a radix sort. Edges near 2^32 sit in a side list; only the queries they can win leave the 32-bit search.

The road graph is sparse, so the two searches meet after a few thousand expansions, and the renumbering keeps those expansions in cache.

## Dependencies

None. Static C++20.

## With another 24 hours

One CSR build instead of the temporary graph used for renumbering, and tighter edge packing so peak RSS stays near the two adjacency arrays.
