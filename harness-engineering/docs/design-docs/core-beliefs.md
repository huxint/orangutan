# Core Beliefs

These beliefs shape Orangutan v2 before any feature-specific decision. They are
non-negotiable; everything else in the repository follows from them.

## Agent-First Operating Principles

- **Humans steer; agents execute.** The repository's job is to make the steering
  cheap and the execution legible.
- **Repository-local knowledge beats private human context.** If a decision lives only
  in chat, it does not exist for the next agent.
- **The right fix for repeated agent failure is usually better scaffolding, not more
  prompt pressure.** A missing doc, a missing script, a missing test — fix the
  *environment* first.
- **Short stable entry points beat large unstable instruction dumps.** `AGENTS.md` is a
  router; the encyclopedia is the `docs/` tree.
- **Mechanical checks over soft conventions.** A rule without a `scripts/check-*.sh`
  enforcement is a rule that will silently rot.
- **Unmanaged entropy compounds.** Cleanup is part of the feature, not after the feature.

## C++23 Engineering Principles

- **Correctness, then maintainability, then performance, then cleverness.** In that
  order. Always.
- **Compile time is a feature.** Every TU has a budget; every PR is responsible for not
  pushing it past the budget. The legacy `orangutan/` failed here, and we are not going
  to make the same mistake. See `../rules/compile-budget.md`.
- **Public headers are contracts.** They are forward-declaration heavy; full includes
  live in `.cpp` files. If you must add a heavy `#include` to a public header, it goes
  through review.
- **No macros for control flow.** Constexpr, concepts, and templates instead. Macros
  exist for include guards and conditional compilation only.
- **No `std::thread`, no custom thread pool, no `new`-managed lifetimes.** Async is asio
  + coroutines; ownership is RAII; raw pointers are non-owning views.
- **No exceptions across library boundaries.** `std::expected<T, Error>` is the cross-
  boundary error type. Exceptions are an internal-only mechanism, contained.
- **Senders, callbacks, futures — choose one and stick to it per library.** The codebase
  has one async vocabulary: asio's `awaitable<T>`. Mixing styles is a smell.

## Module Discipline

- **One public façade per library.** `#include <oran/<lib>.hpp>` is the only sanctioned
  way to consume a library from another library.
- **Modules first, headers second.** When GCC 16.1's module support is stable for a
  shape, prefer `export module oran.<lib>` over header-only.
- **PCH for stable headers only.** The PCH lists stdlib, fmt-fwd, asio-fwd,
  nlohmann_json_fwd, and the `oran::core` public surface — nothing that changes
  often.
- **No transitive heavy includes.** A library's public header does not pull in `json.hpp`,
  `httplib.h`, `spdlog/spdlog.h`, or NVIDIA `stdexec`. Use forward decls or shims.

## Async Belief

- **One executor.** asio's `io_context`, wrapped in `oran::async::Runtime`. Everything
  is a coroutine that suspends on this executor.
- **Cancellation is universal.** Every coroutine accepts an `asio::cancellation_slot` or
  uses `co_spawn`/`async_compose` with one. No fire-and-forget tasks without an explicit
  detachment story.
- **Backpressure is explicit.** Bounded `oran::async::Channel<T>` for cross-coroutine
  queues. Drop policy is documented at the channel site.

## Memory Belief

- **Memory tiers are explicit, not emergent.** Working / session / long-term / shared.
  Each tier has a backend trait, a retention rule, and a hookable lifecycle.
- **The agent reads memory once per turn.** The `oran::prompt` module renders the
  memory section *outside* the ReAct loop, not on every iteration. The legacy code's
  per-iteration memory queries were a measurable cost — see the audit.

## Channel Belief

- **Adapters are interchangeable.** The agent runtime never depends on which channel
  delivered a message. QQ, Discord, Slack, Telegram, Webhook, and any future adapter
  implement the same `oran::channel::Channel` trait.
- **Capability matrices are first-class.** Each adapter declares what it supports
  (attachments, reactions, threads, mentions, ephemeral messages, …). The agent UI
  layer adapts.

## Provider Belief

- **Protocol adapters are dumb.** They map our domain types to a vendor's wire format,
  nothing more. Retry, fallback, observability live in `oran-provider::execution`.
- **Capabilities not provider names.** Code asks `route.supports(Capability::Streaming)`,
  not `route.is_anthropic()`.

## Hook Belief

- **Lifecycle events are enumerable.** Each subsystem publishes a list of named
  lifecycle points (`docs/design-docs/permissions-and-hooks.md`). New points appear via
  rule update, not silently in code.
- **Sinks are pluggable.** Shell, in-process C++, embedded Lua, Wasm — every sink kind
  implements `oran::hook::Sink`.

## Bench Belief

- **`bench/` exists alongside `tests/`** for every library. If a library has no
  benchmark, it has no documented performance contract.
- **A-vs-B comparisons are the default.** When two reasonable implementations exist
  (variant vs. polymorphic; FTS5 vs. vector cosine; coroutine-based queue vs. lock-free
  ring), `bench/<lib>/` ships both and `compare.cpp` reports the tradeoff.
- **CI tracks bench results over time.** Regressions are reviewed like test failures.

## What This All Adds Up To

A v2 codebase where:

- A clean release build finishes in **≤ 30 s on a modern 8-core / 16 GB machine**.
- An incremental build that touches one library's `.cpp` recompiles **≤ 5 s**.
- Adding a new chat platform is **one library**, not a sprawl.
- Adding a new LLM provider is **one protocol adapter**, not a fork of the runtime.
- Memory, hooks, permissions, and benchmarks are uniform — agents can extend them by
  filling templates, not by reverse-engineering.
