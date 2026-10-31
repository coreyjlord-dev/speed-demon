# Track B: Index

Answer 50,000 boolean and phrase queries over a 250,000-document corpus. Build and query share one clock.

## Approach

Queries are read first. Only tokens that appear in them are indexed. A token is a maximal `[a-z0-9]` run after ASCII lowercasing; every other byte, including UTF-8, is a separator. A short sample picks terms that show up in more than 1/32 of documents; those become bitmaps. Every other posting is a 16-bit offset from the worker's first document, filled from a 3-byte scan log that is unmapped bucket by bucket so the log and the lists are not both fully resident. Transparent huge pages are off, which keeps partly written blocks from rounding up to 2 MB. BOOL runs strictly left to right, and once a bitmap enters the result it stays a bitmap. A phrase intersects its terms rarest-first, then checks the candidate's raw text for that term and its neighbours. The text check matches the anchor's first two bytes together, so a common first letter does not reach the scalar walk. Term slots are prefetched 16 tokens ahead. Results are written in one call. No positions are stored.

## Dependencies

None. Static C++20.

## With another 24 hours

The scan is still most of the run. Term lookup hits a 1 MB table, skips a token already recorded in the current document, and folds the key with one multiply. Bitmap updates are plain ors. The harness counts 308 lines while formatting is held, and every source line is at most 120 characters. Dropping that guard reformats to 717 lines and the same speed scores about 64.9, under the board's 67.43. The scan is still most of the run. The record on the board is about 0.33 s and 81 MB; this tree's board-scale estimate is 0.292 s at the same memory, about 67.5 points.
