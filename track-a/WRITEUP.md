# Track A write-up

The reference spends most of its time in `strptime` and per-line Python objects. The replacement never builds a date and never allocates per line.

The mapped file is split on newlines across workers. A line is `27-byte timestamp, method, endpoint, 3-digit status, latency`. The hour index is `day_offset * 24 + hour - 18` inside the published window that starts at 2026-10-30T18:00Z, which the spec fixes for every seed. The endpoint is interned with an open-addressed hash table local to the worker; the same bytes in another worker merge by hash and `memcmp`. Latencies are `uint32` values in a bump arena. A 5xx is `status[0] == '5'`.

Percentiles are nearest rank. For a group of `n`, the value is the element at index `(p * n + 99) / 100 - 1` after `nth_element`, so p99, then p95 inside that prefix, then p50. Error ppm is `errors * 1000000 / count` in integer arithmetic. Output order is hour, then endpoint bytes, with a proper prefix first.

Checked against the spec's worked example, the tiny generator, and the published full-seed sha256 `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.
