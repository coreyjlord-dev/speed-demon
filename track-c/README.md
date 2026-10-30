# Track C: Join

Parse the hostile CSV, join `region_id` to a country, and emit one aggregate line per country that has records.

## Approach

`regions.csv` is plain. Region ids fit in a one-byte array of country indexes; a miss, inside the range or past it, is `_UNMATCHED`. `records.csv` is parsed with the spec's quote rules: `""` is one quote, a quoted LF does not end the record, and a BOM is skipped only at a record boundary. Quotes are found eight bytes at a time. Amounts are integer cents. Each worker interns names into a small arena (the pool is about 100k strings) and appends those ids per country. After the scan, one country at a time is folded into a bitset that fits in L1. A shared intern gives both workers the same id for the same bytes, so the bitsets OR. `name_bytes` and `note_bytes` sum every record, not the distinct set. Pages of the CSV already scanned are dropped.

Workers start on record boundaries. Section 2.2 says an embedded newline in a note is followed by a letter or a quote, never a digit, so a newline followed by an optional BOM and a digit is a real record start. The parser after that point is still quote-aware.

## Dependencies

None. Static C++20.

## With another 24 hours

The scan is still most of the time: about 50 ms locally to touch the CSV, then the intern. The record is 47 ms.
