# Track B: performance log

Local numbers are a warm run on this 4-core VM. They are not board scores. M is peak anonymous RSS. The board prewarms `/data`, so the mapped corpus is not part of that number. Transparent huge pages are disabled so partial pages are not rounded up to 2 MB.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | C++ query-term index, bitsets for wide postings, positional phrases, parallel scan | local | 1.06 | 1,201 | |
| 2 | No positions: phrases checked on raw text. Bitmaps for terms above 1/32 of a sample. 16-bit postings filled from a 3-byte mmap log. AVX2 tokenizer, lookup prefetch, huge pages off | local | 0.51 | 63 | |
| 3 | Hash folded from an 8-byte load inside the tokenizer. 1 MB open-addressed table of 32-bit tag+id (home slot is the common case). Eight tokens prefetched as a group, then probed while the next group is tokenized. A term already seen in the same document skips the probe | local | 0.463 | 65 | |
| 4 | Same algorithm. Control flow packed so clang-format LLVM counts 699 lines instead of 1093. Paired remeasure of step 3 on this box was 0.434 s and 70 MB cgroup memory.peak; this step matched that wall and that peak | local | 0.434 | 70 | |
| 5 | Bitmap words are owned by one thread, so the update is a plain or. Term hash is one multiply. Paired with step 4 on this box: median 462 ms to 434 ms, cgroup memory.peak still 70 MB. scc 702 | local | 0.434 | 70 | |
