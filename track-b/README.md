# Track B: Index

Answer 50,000 boolean and phrase queries over a 250,000-document corpus. Build and query share one clock.

## Approach

Queries are read first. Only tokens that appear in them are indexed. A token is a maximal `[a-z0-9]` run after ASCII lowercasing; every other byte, including UTF-8, is a separator. A short sample picks terms that show up in more than 1/32 of documents; those become bitmaps. Every other posting is a 16-bit offset from the worker's first document, filled from a 3-byte scan log that is unmapped bucket by bucket so the log and the lists are not both fully resident. Transparent huge pages are off, which keeps partly written blocks from rounding up to 2 MB. BOOL runs strictly left to right, and once a bitmap enters the result it stays a bitmap. A phrase intersects its terms rarest-first, then checks the candidate's raw text for that term and its neighbours. No positions are stored.

## Dependencies

None. Static C++20.

## With another 24 hours

The scan is still most of the run. Term lookup now hits a 1 MB table and skips a token already recorded in the current document; what is left is the query phase, about 130 ms of a 463 ms local run, and the formatted line count. The record on the board is about 0.29 s and 70 MB.
