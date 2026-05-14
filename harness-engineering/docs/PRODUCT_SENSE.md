# Product Sense

This file captures the product principles that help agents make good tradeoffs
without constant prompting.

## Primary Users

1. **Solo developer** — wants a local coding assistant on their workstation.
   Cares about: low latency, low cost, file/shell tooling, session continuity.
2. **Small team** — wants a single runtime that exposes the same agent through CLI,
   web, and a chat channel.
   Cares about: shared memory, audit trail, permissioned tool use.
3. **Self-hosted product team** — runs the binary on a VM, exposes web + channels,
   automates recurring tasks.
   Cares about: reliability, scheduled jobs, multi-channel reach, supply-chain
   posture.

## What Makes The Product Useful

- **Local first.** Runs on a developer's machine without external services beyond
  the LLM provider.
- **Pluggable.** Channels, providers, hook sinks, tools, memory backends, strategies
  — each is a small extension surface, documented under `docs/design-docs/`.
- **Auditable.** Every effectful action goes through permissions and is recorded.
- **Fast to iterate on.** Compile time, build system, test/bench parity make it
  pleasant to extend.
- **No "kitchen sink".** Each library has a tight scope; new features earn their place.

## Quality Attributes (Ordered)

1. **Correctness.** No silent broken behavior; errors surface with context.
2. **Maintainability.** Future you / next agent / next operator can reason about
   it without reading a thousand-line file.
3. **Operational legibility.** Logs, audit, metrics convey what happened.
4. **Compile time.** A core feature — see [`FAST_COMPILATION.md`](FAST_COMPILATION.md).
5. **Runtime performance.** Important on hot paths (agent iteration overhead, tool
   dispatch); not the first priority elsewhere.
6. **Polish.** UX touch-ups (terminal colors, web UI styling) come last in v1.

## Tradeoffs To Default

| When two options conflict… | Default |
| --- | --- |
| Velocity vs. cleanliness | Velocity now, debt entry now. |
| New feature vs. existing simplification | Simplify first if the area is messy; otherwise stage. |
| Generality vs. specificity | Specific. Generalize when a second use case appears. |
| Performance vs. clarity | Clarity, unless a bench shows a meaningful hotspot. |
| Latency vs. consistency | Consistency. The runtime is for humans; ms-level latency rarely matters. |
| New dependency vs. small inline implementation | Inline if < 200 LoC and self-contained. |

## What Not To Build

- Multi-tenant SaaS features.
- High-QPS public-facing API.
- "Smart routing" that auto-selects models based on prompt; routes are config.
- A plugin system more elaborate than channel adapters / hook sinks / strategies.

## See Also

- [`design-docs/agent-platform.md`](design-docs/agent-platform.md) — vision.
- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — current state.
