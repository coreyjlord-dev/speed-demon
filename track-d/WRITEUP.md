# Track D write-up

The reference runs a fresh binary-heap Dijkstra per query and, on an unreachable target inside the main component, settles the whole graph. This program does three things before the search: collapse parallel edges to the lightest weight, drop self-loops, and answer the trivial queries. `s == t` is 0 even if the node has no edges. If `s` has no outgoing edge or `t` has no incoming edge and `s != t`, the answer is -1.

Everything else is bidirectional Dijkstra. Forward uses the outgoing CSR, backward the incoming CSR. A label is the length of a real path, so when a node is labeled from both sides the sum is a feasible s-t walk. The search expands whichever side has settled fewer nodes and stops when `min_forward + min_backward` is at least that best walk, or when either heap runs dry. That is what keeps an unreachable island from dragging the main component along with it. Weights are non-negative, including zero, so the first time a node is popped at its current label it is settled. Stale heap entries are ignored. Distances are `uint64` because a path can exceed `2^32 - 1`.

The heap is a radix heap: bucket 0 holds every label equal to the last extracted minimum, and a larger label is filed by the highest bit in which it differs. Workers do not share heaps or label arrays.

Checked against the spec's worked example, tiny generator output, and the published full-seed sha256 `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e`.
