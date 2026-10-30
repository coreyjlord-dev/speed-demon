# Track B: Index

Answer 50,000 boolean and phrase queries over a 250,000-document corpus. Build and query share one clock.

## Approach

Queries are read first. Only tokens that appear in them are indexed. A token is a maximal `[a-z0-9]` run after ASCII lowercasing; every other byte, including UTF-8, is a separator. Workers tokenize newline-aligned slices into per-thread posting arenas, then the slices are concatenated so document ids stay sorted. Wide postings (8,192 docs or more) also become bitsets. BOOL runs strictly left to right: AND, OR, and NOT on the smaller representation. A phrase walks positions of its rarest term and checks the neighbouring offsets. Each answer is the count, the sum of ids, and the ten smallest ids.

## Dependencies

None. Static C++20.

## With another 24 hours

The scan still hashes nearly every token, because most tokens are query terms under this distribution. A SIMD tokenizer and a tighter positional layout are the next levers. The record is 270 ms.
