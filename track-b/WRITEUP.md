# Track B write-up

The reference keeps a Python set per vocabulary term and re-tokenizes phrase candidates from raw text. This program indexes only the terms the queries actually name, which on seed 1 is about 76,000 terms and still most of the token stream, then answers from that index.

Tokenization matches `re.findall(rb"[a-z0-9]+", text.lower())` on bytes: `A-Z` fold by OR-ing `0x20`, and bytes at or above `0x80` stay separators, so `Zürich` is `z` and `rich`. Document ids are appended in order inside each worker and workers cover increasing id ranges, so merged postings are sorted and unique per document. Phrase terms also store token positions. A phrase picks the term with the fewest positions and tests `start + offset` for the others, so a repeated term has to repeat in the document. BOOL does not reorder operators. `a OR b AND c` is `(a OR b) AND c`. Bitsets cover the wide lists; rare lists stay sorted vectors, and AND of a short list against a long one binary-searches.

Empty matches print `0 0`. Checked against tiny generator output and the published full-seed sha256 `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927`.
