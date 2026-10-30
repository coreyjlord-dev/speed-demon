# Track C write-up

Correctness is the parser. A field that starts with `"` ends at the next quote that is not part of `""`, and the value is the interior with each `""` replaced by one quote. An unquoted field is raw bytes up to the comma. CRLF and LF both end a record, and only outside quotes. A BOM (`EF BB BF`) is ignored at the start of the file and immediately after a record terminator. Anywhere else, including inside quotes, those bytes would be data; the generator does not put them there.

The join key is the integer `region_id`. Countries are three ASCII letters packed into a dense index. Aggregates are count, id sum, amount sum, min, max, distinct names, name bytes, and note bytes. Money is printed from integer cents, so `-5` cents is `-0.05` and zero is `0.00`, never `-0.00`. Empty quoted and empty unquoted names are the same empty string. `Betts` and `Betts ` are different. A country with no records is omitted. `_UNMATCHED` sorts last because `_` is after `Z`.

Checked against the spec's worked example, tiny generator output, and the published full-seed sha256 `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536`.
