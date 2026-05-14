# 0008 — Permissions Engine

## User Problem

A runtime that can write files and run shell is a runtime that can destroy a machine.
Operators need fine-grained, audited control over what the agent may do, with explicit
human approval for high-risk operations.

## Scope (v1)

- `oran-permission::Evaluator` with modes: `auto`, `default`, `permissive`, `strict`,
  `sandboxed`.
- Rule types: `allow`, `deny`, `ask`.
- Runtime regex via `re2` (not compile-time ctre).
- Approval prompts HMAC-signed with a per-process secret; replay limited by TTL +
  count.
- Capability-aware gating (rules can match by `Capability`, not just tool name).
- Audit log in `audit.db` for every decision (allow / deny / ask / approved /
  rejected).

## Scope (v1.1)

- Per-channel permission overlays — restrict what the agent may do when a request
  arrived via specific channels (e.g. less trusted external webhook).
- Time-bound approvals — "allow for next 1 hour".
- Sticky approvals — "always allow this exact input shape".

## Scope (v2)

- Approval routing to external channels (Slack, email) for human sign-off.
- Role-based permissions for multi-user runtimes.

## Out Of Scope

- Per-process kernel sandboxing (seccomp, etc.) — relies on external sandbox tools.
- Cryptographic attestation of approvals across machines.

## Acceptance Criteria

1. A tool call whose input matches a `deny` rule returns
   `Error::permission_denied` and is recorded in audit.
2. A tool call whose input matches an `ask` rule renders an approval prompt; on
   approval, replay works within TTL for identical input.
3. Capability mismatch is enforced — a tool that didn't declare `Capability::network`
   cannot use it even if a rule otherwise allowed.
4. `re2` patterns load from config; invalid patterns at load time are reported with
   line numbers.
5. Approval signing key is rotated when the runtime restarts; prior approvals are
   invalidated.
6. `tests/permission/` ≥ 90% coverage including table-driven tests over modes ×
   rule kinds × capability flags.

## Design Doc Cross-References

- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
- [`../design-docs/secrets-and-state.md`](../design-docs/secrets-and-state.md)

## Risks

- Misconfigured rules silently broaden permissions — schema validation + a
  "rule explain" CLI subcommand mitigate.
- Approval prompts grow stale and annoying — TTL + sticky approvals address this.

## Validation

```sh
xmake build oran-permission
xmake test test-permission
xmake run orangutan -- --explain-rules
```
