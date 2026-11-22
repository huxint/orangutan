# bench-tool

`bench-tool` measures the cost of routing a tool call through `tool::Registry`.

A-vs-B comparisons:

- `registry.lookup`: definition lookup through `Registry::find("noop")`.
- `registry.dispatch_allow`: full `dispatch` walk — permission evaluator
  with a single matching `allow` rule, an in-memory `RecordingAuditSink`
  record, and a trivial in-process handler. The delta over `registry.lookup`
  is the dispatch overhead the agent loop pays per tool call.
- `file_write.dispatch_truncate` vs. `file_write.dispatch_append`: full
  `FileWrite` dispatch through the registry under each `WriteMode`,
  hitting a real 64-byte write to a tempfile. The two scenarios share
  every cost (permission eval + SHA-256 input hash + audit record + JSON
  parse + disk IO); the contrast measures whether the mode choice has
  any measurable effect at small payload sizes. This is the baseline a
  future "buffered vs. unbuffered" or "direct vs. cached" write
  optimization would need to beat.
- `file_edit.dispatch_unique_replace` vs.
  `file_edit.dispatch_replace_all_many`: full `FileEdit` dispatch on a
  1 KiB seed file. The unique-replace case finds and rewrites one
  occurrence; the replace-all case rewrites 64 occurrences spaced every
  16 bytes. Both scenarios pay the same per-call overhead (permission
  eval + SHA-256 + audit + JSON parse + read + write); the contrast
  measures whether the per-match substitution cost is material against
  the bulk read/write at typical edit sizes. The baseline a future
  rope or in-place rewrite would need to beat.
- `dispatch_ask_short_circuit` vs. `dispatch_ask_approved` vs.
  `dispatch_ask_rejected`: three-way contrast of the `Verdict::ask`
  dispatch paths added in slice 21. The short-circuit case carries
  neither `approval_broker` nor `approval_token` on the context — same
  cost as the slice-17/.../slice-20 ask path plus the new
  `replay_max` / `approval_ttl_seconds` / `decision_reason` error-
  context build. The approved case attaches a broker + token (with
  effectively-infinite `replay_max`) so `broker.check` succeeds, the
  audit row records `outcome=approved`, and the trivial in-process
  handler runs. The rejected case attaches a broker + an exhausted
  token (`replay_max=0`) so `broker.check` returns
  `reason=replay_exhausted`, the audit row records `outcome=rejected`,
  and the handler is skipped. The (approved − short_circuit) delta is
  the per-call broker-attached overhead the agent loop pays once it
  has an approval in hand (~11 µs, dominated by the HMAC verify);
  (rejected − short_circuit) is the per-call cost of a stale or
  exhausted token (similar shape — the broker does the same MAC work
  before deciding the entry is missing or out of budget).
- `dispatch_allow_no_hooks` vs. `dispatch_allow_with_empty_bus` vs.
  `dispatch_allow_with_two_sinks`: three-way contrast of the slice-22
  hook-bus wiring on the allow path. The no-hooks case sets
  `DispatchContext::bus = nullptr` — the slice-17 baseline. The
  empty-bus case sets `bus` to a real `hook::Bus` with no sinks
  subscribed; dispatch still pays the two `publish_advisory` map
  lookups but each returns immediately. The two-sinks case binds
  one callback sink each to `tool_before` and `tool_after`, so
  dispatch awaits the two sink coroutines. The
  (with_empty_bus − no_hooks) delta is the "bus attached but nothing
  listens" cost the agent loop pays to keep the bus wired; the
  (with_two_sinks − no_hooks) delta is the per-call cost of a single
  observer subscribed to both bookend events. Both should be sub-µs
  — the cost of an expensive sink (shell, webhook) is the sink's own
  bill, not the bus's.
- `catalog.select_all_32_tools` vs. `catalog.select_subset_32_tools`:
  select every definition or four explicit names from a 32-tool snapshot.
  Both paths return owned definitions sorted by name. Selection does not parse
  schemas or retain a rendered-block cache.
- `output.text_only` vs. `output.with_data_16kib`: construction cost for
  the v1-compatible text-only envelope and for an envelope that carries a
  16 KiB serialized structured payload plus usage counters. This pins the
  fixed overhead of the spec-0014 envelope before provider adapters start
  serializing it into vendor tool-result shapes.
- `output.apply_caps`: cost of applying spec-0014 output byte caps to an
  oversized text fallback plus oversized structured payload. The scenario
  covers the dispatch/scheduler helper that truncates text and drops
  `data_json` before hook/provider-facing output leaves the tool boundary.
