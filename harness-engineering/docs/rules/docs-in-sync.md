# Docs In Sync — The Prime Directive

> **Every change updates the docs that describe it. Same PR. No exceptions.**

This is the most-broken rule in C++ projects: code drifts, docs lag, the next person
opens a five-year-old README and trusts it. We refuse to repeat that pattern.

## The Rule

If a PR changes any of the following, it **must** update the matching docs in the
same change:

| Change                                              | Docs to update (at minimum)                          |
| --------------------------------------------------- | ---------------------------------------------------- |
| New / removed library                               | `docs/ARCHITECTURE.md` (library inventory), `docs/rules/libraries.md` if 3rd-party, `docs/rules/compile-budget.md` (category row), `docs/QUALITY_SCORE.md` row |
| New / changed public API on an existing library     | matching `docs/design-docs/<area>.md`, the library's umbrella header docstring, and (if user-visible) `docs/product-specs/<id>.md` |
| New / changed configuration field                    | `config.example.json`, the relevant `docs/design-docs/secrets-and-state.md` or `docs/design-docs/<area>.md`, and `docs/RELIABILITY.md` if it affects ops |
| New / removed CLI flag or subcommand                 | `docs/product-specs/0001-core-react-loop.md` (or the relevant spec), README quick-start if user-visible, `docs/CICD.md` if CI-impacting |
| New / removed environment variable                   | `docs/RELIABILITY.md` "Required environment" table |
| Build system change (xmake target, package bump, option) | `docs/BUILD_SYSTEM.md`, `docs/rules/libraries.md` (versions), `docs/rules/compile-budget.md` if compile cost shifts |
| Compile-time budget movement                          | `compile_budget.json` and a paragraph in `docs/rules/compile-budget.md` if the cap moved |
| Module / PCH change                                    | `docs/rules/module-and-pch.md`, `docs/generated/pch-spec.json` |
| New / changed hook lifecycle event                     | `docs/design-docs/permissions-and-hooks.md` (canonical event list) |
| New / changed permission capability                    | `docs/design-docs/tool-runtime.md`, `docs/design-docs/permissions-and-hooks.md`, `docs/product-specs/0008-permissions.md` |
| New / changed memory tier, backend, or kind            | `docs/design-docs/memory-system.md`, `docs/product-specs/0005-memory-system.md` |
| New / changed channel adapter or capability            | `docs/design-docs/channel-abstraction.md` (capability matrix), `docs/product-specs/0003-multi-platform-channels.md`, optionally `docs/design-docs/channel-<name>.md` |
| New / changed provider protocol adapter                | `docs/design-docs/api-portability.md`, `docs/product-specs/0001-core-react-loop.md` if scope-affecting |
| New / changed orchestration strategy                   | `docs/design-docs/team-collaboration.md`, `docs/product-specs/0004-agent-team.md` |
| New / changed automation category                      | `docs/design-docs/<area>.md`, `docs/product-specs/0006-automation.md` |
| New / changed file layout convention                   | `docs/ARCHITECTURE.md` "Intended Repository Shape", relevant README under `include/`, `src/`, `tests/`, `bench/`, `skeleton/` |
| New / changed test or bench bucket                      | `tests/README.md` or `bench/README.md`, plus `docs/rules/testing-and-bench.md` if conventions shift |
| New rule, removed rule, or relaxed enforcement         | the affected `docs/rules/*.md`, `docs/rules/README.md` table |
| New script under `scripts/`                              | The README that references it, plus `Makefile` if it's a make target |
| New CI workflow / job                                    | `docs/CICD.md`, `docs/SUPPLY_CHAIN_SECURITY.md` if applicable |
| Anything user-visible                                    | `docs/releases/feature-release-notes.md` |
| Any behavior change at all                               | `docs/histories/YYYY-MM/YYYYMMDD-HHmm-<slug>.md` |

If the table doesn't list your change, ask "what doc would a new agent want to read
about this in six months?" — and update *that* doc.

## What "Same PR" Means

- Code and doc edits land in the same commit, or at minimum in the same PR.
- A doc-only follow-up PR is not acceptable as the close-out for a code PR.
- Reviewers must reject PRs that change behavior without matching doc edits.

## What "Match Reality" Means

- A doc that *describes* a class signature must list the actual signature in code.
- A doc that *enumerates* lifecycle events must list every event the code fires.
- A doc that *quotes* configuration shape must match `config.example.json`.
- A doc that *names* a script, target, env var, or path must match what exists.
- A doc that *promises* a budget, behavior, or metric must reflect what CI measures.

If you cannot honor the description, **change the description**, do not leave it
broken.

## Mechanical Enforcement

- `scripts/check-docs.sh` — required-file existence (already runs in CI).
- `scripts/check-repo-hygiene.sh` — hygiene files (already runs in CI).
- `scripts/check-docs-sync.sh` — additional drift checks (this rule's enforcer):
  - Public-header symbol names referenced in `docs/design-docs/*.md` must exist in
    `include/oran/`.
  - Config fields named in `docs/design-docs/secrets-and-state.md` must appear in
    `config.example.json`.
  - Lifecycle event names enumerated in `docs/design-docs/permissions-and-hooks.md`
    must match the code's `Event` enum.
  - Capability names enumerated in `docs/design-docs/tool-runtime.md` must match the
    code's `Capability` enum.
  - Library names in `docs/ARCHITECTURE.md` inventory must match `xmake/targets.lua`.
  - Package versions in `docs/rules/libraries.md` must match `xmake/packages.lua`.
  - Required files listed in `docs/rules/README.md` must exist.
- CI fails any PR that fails the above.

The script reports each drift with the exact pair of files that disagree and a
suggested fix.

## What An Acceptable PR Looks Like

```text
git status (typical)
  modified:   src/oran-tool/registry.cpp
  modified:   include/oran/tool/registry.hpp
  modified:   docs/design-docs/tool-runtime.md            # API doc updated
  modified:   docs/product-specs/0002-tool-registry.md    # acceptance criterion updated
  modified:   tests/tool/registry_lookup.cpp              # test added
  modified:   bench/tool/dispatch_overhead.cpp            # bench added
  new file:   docs/histories/2026-05/20260520-1430-tool-batch-dispatch.md
  modified:   docs/releases/feature-release-notes.md      # user-visible behavior change
```

If your `git status` doesn't include doc edits when the code edits invalidate them,
the PR is incomplete.

## Counter-Examples (Rejected At Review)

- Code adds a new `provider.cost_threshold` hook event, but
  `docs/design-docs/permissions-and-hooks.md` still lists the old enum.
- A package version bump in `xmake/packages.lua` without updating
  `docs/rules/libraries.md`.
- A new `--strict-config` CLI flag without a paragraph in the relevant spec / README.
- Renaming `oran-tool-orchestration` → `oran-tool-team` without updating
  `docs/ARCHITECTURE.md` and `docs/rules/libraries.md` and the dependency tables.
- Removing the `oran-channel-discord` adapter without removing it from
  `docs/design-docs/channel-abstraction.md` and `docs/product-specs/0003-multi-platform-channels.md`.

## What Counts As An Exception?

There are *no* exceptions for code drift. There is *one* limited exception for
*scaffolding work that has not yet shipped*: changes inside `docs/exec-plans/active/`
may legitimately describe a future state. Once the plan moves to
`docs/exec-plans/completed/`, the production docs **must** describe the actually-shipped
state.

## What If The Right Doc Doesn't Exist Yet?

Create it in the same PR. The right time to write the doc that describes a feature is
when the feature lands. Waiting for a "docs sprint" is how docs rot.

If you don't know where to put the new doc, default to `docs/design-docs/` for a
design topic, `docs/product-specs/` for a user-visible feature, or
`docs/references/` for distilled external material.

## What If The Change Is Trivial?

A pure typo fix in code, or a single-line refactor that doesn't change any externally
visible behavior, does not need a doc edit beyond a history entry — and even that is
overridable with `History-Skip: <reason>` in the PR description. **Behavior change of
any kind** requires the matching doc edit.

## Why The Rule Exists

The previous `orangutan/` codebase had:

- A `CLAUDE.md` that referenced `.claude/rules/` files that didn't exist.
- A `tools/runtime-loader/` mentioned in code comments that had been removed.
- A `qq_channel` option whose semantics differed between the option doc and the actual
  build.
- A "SQLite migration to expected API" goal stated in the docs and only half done in
  code (~120 throwing callsites unmigrated).

Every one of those is a *normal* outcome of an "I'll update the doc later" culture.
The Prime Directive is the only known cure.

## See Also

- [`critical-rules.md#C16`](critical-rules.md) — the rule line.
- [`../REPO_COLLAB_GUIDE.md`](../REPO_COLLAB_GUIDE.md) — "Documentation Discipline".
- [`../HISTORY_GUIDE.md`](../HISTORY_GUIDE.md) — finished-task records.
- [`../PLANS_GUIDE.md`](../PLANS_GUIDE.md) — when intent precedes code.
