# Design Docs Index

This directory holds the **deep architectural designs** for Orangutan v2. Each file
covers one design area; files cross-link rather than duplicate.

If you are starting a non-trivial task, the rule is:

1. Read the matching doc here.
2. If reality has drifted from the doc, fix the doc in the same change.
3. If you need a *new* design area, start a doc with a single paragraph stating the
   problem, then write the design.

## Catalogue

### Foundational

- [`core-beliefs.md`](core-beliefs.md) — non-negotiable operating principles for the
  whole codebase.
- [`module-boundaries.md`](module-boundaries.md) — what's allowed to depend on what;
  how to keep TUs small; one-way dependency rule.
- [`async-model.md`](async-model.md) — executor topology, coroutines, cancellation,
  backpressure, why no `stdexec`.

### Agent Runtime

- [`agent-platform.md`](agent-platform.md) — the vision for the runtime: what kinds of
  agents, what kinds of platforms, what kinds of integrations. Read before designing
  new top-level features.
- [`tool-runtime.md`](tool-runtime.md) — tool registry, dispatch, permission/hook
  ordering, deferred-tool discovery.
- [`memory-system.md`](memory-system.md) — working / session / long-term / shared
  memory tiers, backends, retention.
- [`api-portability.md`](api-portability.md) — provider abstraction, protocol adapters,
  capability matrix.
- [`team-collaboration.md`](team-collaboration.md) — multi-agent coordination strategies,
  mailbox, shared scratchpad, conversation DAG.

### Platform

- [`channel-abstraction.md`](channel-abstraction.md) — the `Channel` trait + per-adapter
  capability matrix.
- [`permissions-and-hooks.md`](permissions-and-hooks.md) — runtime permission engine and
  the hook bus across every subsystem.
- [`secrets-and-state.md`](secrets-and-state.md) — secret encryption, database file
  layout, migrations, retention.

## Seed Documents (legacy continuity)

These files distill what we keep from `orangutan/` and what we deliberately replace:

- [`../references/orangutan-legacy-audit.md`](../references/orangutan-legacy-audit.md)
- [`../references/harness-template-distill.md`](../references/harness-template-distill.md)
- [`../references/third-party-libs.md`](../references/third-party-libs.md)
