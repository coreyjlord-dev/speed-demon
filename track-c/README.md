# Track C: Join

Parse the hostile CSV, join `region_id` to a country, and emit one aggregate line per country that has records.

## Approach

`regions.csv` is plain. Region ids fit in a direct array of country indexes; a miss, inside the range or past it, is `_UNMATCHED`. `records.csv` is parsed with the spec's quote rules: `""` is one quote, a quoted LF does not end the record, and a BOM is skipped only at a record boundary. Amounts are integer cents. Names are interned as exact unescaped bytes. Each worker sets a bit per `(country, name id)`; the bits are unioned at the end, so the same name in two workers counts once. `name_bytes` and `note_bytes` sum every record, not the distinct set.

Workers start on record boundaries. Section 2.2 says an embedded newline in a note is followed by a letter or a quote, never a digit, so a newline followed by an optional BOM and a digit is a real record start. The parser after that point is still quote-aware.

## Dependencies

None. Static C++20.

## With another 24 hours

A SIMD scan for quotes and newlines. Distinct names are already a bitset; the remaining time is touching every CSV byte. The record is 51 ms.
