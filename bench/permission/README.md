# `bench-permission`

Benchmarks cover rule evaluation, defaults, input matching, approval signing,
grant reuse and audit recording. Configuration-to-rule compilation belongs
to [bench-bootstrap](../bootstrap/README.md).

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/rule_set.cpp`](scenarios/rule_set.cpp) | `permission::evaluate` precedence walk (deny → allow → ask, three passes) over a 16-rule fixture *vs.* `std::ranges::find_if` single-pass first-match scan on the same rules. The precedence-respecting walk does up to three passes plus a formatted-reason build; the find_if path is the cheapest possible matcher and ignores precedence. Documents the cost of the foundation evaluator. The same file also registers `permission.rule_set_capability_match` *vs.* `permission.rule_set_capability_miss` over a separate 16-rule capability-scoped fixture: the match path fires a capability-bound rule the call satisfies (formats a `capability=<name>` reason); the miss path runs the same precedence walk but every capability-bound rule's scope excludes the call, so the walk falls through to the mode default. Documents the cost of the optional-capability check on both the success and miss sides. |
| [`scenarios/defaults.cpp`](scenarios/defaults.cpp) | `default_rules(Mode::default_)` factory call *vs.* an inline build of the same 9-rule baseline. Documents the cost of the factory (one function-call frame + one `RuleSet` move out) against bare inline construction so future config-loading paths know the factory is essentially free at startup. |
| [`scenarios/input_pattern.cpp`](scenarios/input_pattern.cpp) | Three scenarios over a single-rule fixture (`ShellExec` + `deny`): `permission.input_pattern_match` evaluates the rule with an input the re2 pattern accepts (pays re2's `PartialMatch` on the success path plus the formatted reason build); `permission.input_pattern_miss` evaluates the same rule with non-matching input (pays `PartialMatch` on the failure path and the shorter "default by mode=" fallback reason); `permission.no_input_pattern` evaluates a rule shape that drops the `input_pattern` altogether (anchors the cost *removed* by skipping the re2 hop). Together they document the input-regex budget end-to-end. |
| [`scenarios/approval_secret.cpp`](scenarios/approval_secret.cpp) | Four scenarios over a fixed 32-byte key: `permission.hmac_short_message` MACs a 32-byte payload (realistic approval-token size); `permission.hmac_long_message` MACs a 1 KiB payload (per-byte hash cost growth axis); `permission.hmac_macs_equal_ok` runs `ApprovalSecret::macs_equal` on two identical 32-byte MACs; `permission.hmac_macs_equal_no` runs it on a pair that differs in the last byte, anchoring the constant-time-compare guarantee that match and miss cost the same. Together they document the libsodium HMAC-SHA-256 budget end-to-end, which the future approval-token slice will build on. |
| [`scenarios/approval.cpp`](scenarios/approval.cpp) | Three scenarios over `ApprovalAuthority`: `permission.approval_issue` covers the full sign path (SHA-256 over input + random nonce + canonical-bytes assembly + HMAC-SHA-256); `permission.approval_verify_ok` covers the corresponding verify path on a round-tripping token (SHA-256 over input + canonical-bytes rebuild + HMAC + constant-time compare); `permission.approval_verify_expired` exercises the early-reject path where the expiry check fires before any hashing happens. Together they document the criterion-5 approval budget so the future broker / agent-loop wiring slices can compare against a baseline. |
| [`scenarios/approval_broker.cpp`](scenarios/approval_broker.cpp) | Four scenarios over `ApprovalBroker`, the stateful replay-window layer on top of `ApprovalAuthority`. `permission.broker_approve` pays the authority's full `issue` cost plus one map insert/overwrite (steady-state grant cost). `permission.broker_check_ok` pays full `verify` plus one map find + counter decrement (steady-state honored replay cost); `replay_max` is set to a deliberately high value so the inner loop never exhausts. `permission.broker_check_no_grant` and `permission.broker_check_exhausted` document the two broker-only rejection paths the authority can't see: full verify followed by a missing-entry vs. zero-counter `Error` build. Together they bound the per-call broker overhead and pin the cost of the two replay-window-only rejection reasons. |
| [`scenarios/audit.cpp`](scenarios/audit.cpp) | Four scenarios over the audit sink hierarchy. `permission.audit_null_sink` is the floor (`NullAuditSink::record` = coroutine glue + virtual call only); `permission.audit_recording_sink` adds one `vector::push_back` (in-memory test/diagnostic sink); `permission.audit_storage_sink` walks the full path through `storage::AuditRepository::append_event` so the steady-state SQLite-backed audit cost is pinned; `permission.audit_to_hex_32_bytes` documents the per-event hex-encode cost the storage sink pays on every event that carries an `input_hash`. |

## Running

```sh
xmake build bench-permission
xmake run bench-permission
```

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
