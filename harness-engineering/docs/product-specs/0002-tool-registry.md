# 0002 — Tool Registry + Built-in Tools

## User Problem

Operators want the agent to do useful work: read & write files, search code, run
shell commands, recall facts, message teammates, schedule jobs. The tool registry is
the surface where these capabilities are presented, gated, observed, and extended.

## Scope (v1)

- `oran-tool::Registry` with the dispatch contract from `tool-runtime.md`.
- Built-in libraries, each registering its tools at bootstrap:
  - `oran-tool-file` — `file.read`, `file.write`, `file.edit`, `file.search`.
  - `oran-tool-shell` — `shell.exec`, `shell.glob`, `shell.ls`, `shell.move`.
  - `oran-tool-memory` — `memory.recall`, `memory.remember`, `memory.forget`.
- Capability declarations on every built-in.
- `tool-search` (deferred-tool discovery) — non-deferred tool that returns schemas for
  deferred tools on demand.
- Output redaction via runtime regex from config.
- Hook lifecycle wired (tool_before / tool_dispatched / tool_after / tool_error).

## Scope (v1.1)

- `oran-tool-orchestration` — once spec 0004 lands.
- `oran-tool-automation` — once spec 0006 lands.
- `oran-tool-skill` — once spec 0009 lands.
- `oran-tool-mcp` — external MCP client.
- `oran-tool-background` — async job orchestration.
- `oran-tool-attachments` — message attachment management.
- `oran-tool-runtime-loader` — hot reload of tool libraries (stretch).

## Out Of Scope

- Per-platform shell sandboxing beyond uid/path restrictions (Linux only in v1).
- Plug-in tool loading from arbitrary shared libraries (deferred to v2).

## Acceptance Criteria

1. `Registry::add` rejects duplicate tool names.
2. `Registry::dispatch` follows the canonical ordering: hook before → permission →
   ask flow if applicable → hook dispatched → handler → hook after.
3. A tool with `Capability::read_file` declared cannot call `runtime.workspace()` if
   the permission engine did not grant it (returns `Error::capability_not_granted`).
4. `tool-search` returns the schema of any deferred tool within 10 ms.
5. `file.edit` performs structured patches with conflict detection (returns a
   typed error when the file changed underneath).
6. `shell.exec("rm -rf /")` is denied by default permission rules; audit log records
   the attempt.
7. `bench/tool/dispatch_overhead` reports < 50 µs median dispatch path (excluding
   handler).
8. `tests/tool/` coverage ≥ 80%.

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)

## Risks

- Permission/hook coupling makes dispatch testing complex — provide
  `tests/test-helpers/MockPermissionEvaluator` and `MockHookBus`.
- File-edit tool's patch format must be portable across LLM providers — pick a JSON
  shape similar to Claude's `str_replace_editor` for familiarity.

## Validation

```sh
xmake build oran-tool oran-tool-file oran-tool-shell oran-tool-memory
xmake test test-tool
scripts/bench-compare.sh tool
```
