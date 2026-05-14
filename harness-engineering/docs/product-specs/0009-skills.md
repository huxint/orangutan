# 0009 — Skills

## User Problem

Some prompts are reusable: "review a PR", "summarize a doc", "write a release note".
Encoding these as **skills** — short markdown templates with metadata — lets the agent
activate them on demand without bloating the system prompt.

## Scope (v1)

- `oran-skill::Loader` reading skills from `<workspace>/.orangutan/skills/<name>.md`.
- Skill metadata in YAML frontmatter:
  - `name`, `description`, `triggers` (semantic intents), `inputs` (schema), `model_hint`.
- Skill catalog rendered into the system prompt (compact listing).
- `skill.invoke(name, inputs)` tool runs a skill; its body is appended to the
  prompt for the next iteration.
- Hot-reload via filesystem watcher (`asio` + inotify on Linux).

## Scope (v1.1)

- Skill chaining: a skill can declare follow-up skills it expects to be invoked.
- Skill-specific tool subset: a skill can restrict which tools may be used while it's
  active.
- Per-agent skill enablement (skills listed in `agent.<name>.skills_enabled`).

## Scope (v2)

- Cross-runtime skill registry (a shared bucket of skills auto-pulled).
- Skill versioning + immutability (similar to immutable container images).

## Out Of Scope

- Skill marketplaces or remote skill repositories.
- Code-execution skills (a skill is markdown, not arbitrary code).

## Acceptance Criteria

1. A skill placed under `<workspace>/.orangutan/skills/release-note.md` appears in the
   agent's skill catalog within 1 s.
2. `skill.invoke("release-note", { since: "v1.2", ... })` runs the skill and the
   agent produces output consistent with the skill template.
3. Removing the skill file from disk removes it from the catalog within 1 s (no
   restart).
4. Skill activation is observable via the `tool_after` hook with
   `tool_name = "skill.invoke"`.
5. `tests/skill/` ≥ 80% coverage.

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)

## Risks

- A misbehaving skill bloats the prompt — enforce a per-skill body size cap (default
  4 KiB).
- Skill file changes during a turn cause inconsistency — snapshot the skill body at
  invocation time.

## Validation

```sh
xmake build oran-skill
xmake test test-misc-services
```
