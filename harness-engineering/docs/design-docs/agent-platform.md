# Agent Platform — Vision

Orangutan v2 is **a runtime for agents**, not "a chatbot with tools". This document
captures the platform vision that every other design doc must serve. It is intentionally
slightly aspirational; nearer-term scope is captured in `docs/product-specs/`.

## What an Agent Is, In This Codebase

An **agent** is a typed configuration of:

- A **base model** + protocol (Anthropic Messages, OpenAI Responses, etc.) with fallbacks.
- An **identity scope** — keys that namespace memory and audit logs.
- A **toolset** — subset of the global `oran-tool` registry, possibly with per-tool
  permission overrides.
- A **memory profile** — which tiers are visible (working always; session/long-term
  optional per agent; team-shared if the agent is in a team).
- A **skill set** — markdown skills under `<workspace>/.orangutan/skills/`.
- A **hook bindings** map — which hook sinks subscribe to which lifecycle events for
  this agent.
- A **mailbox address** — `<team>/<agent-name>`.

Agents are **plural**: a running process can host dozens of them. Each is driven by an
`oran::agent::Loop` that owns a single ReAct loop. Multiple loops share the executor and
the platform layer; they never share mutable state without a typed channel.

## Where Agents Come From

Three call sites instantiate an agent:

1. **Primary CLI** — `orangutan` started interactively or single-shot.
2. **Orchestrated worker** — `oran-orchestration` spawns a worker under a leader's
   coordination strategy.
3. **Automation firing** — `oran-automation` fires a cron / periodic / triggered job
   and constructs a fresh runtime for the firing.

Each call site uses the same factory in `oran-bootstrap::RuntimeAssembler`. New call
sites are added only by extending the assembler, never by re-implementing it.

## Interfaces — What Talks To Agents

| Interface | Direction       | Bound to                             |
| --------- | --------------- | ------------------------------------ |
| CLI       | bidirectional   | stdin/stdout (REPL) or one-shot      |
| Web UI    | bidirectional   | `oran-web` HTTP + SSE                |
| Channel   | bidirectional   | `oran-channel` (QQ, Discord, Slack, …) |
| Automation | inbound         | `oran-automation` scheduler          |
| Heartbeat | inbound         | internal liveness pings              |
| Orchestration mailbox | bidirectional | `oran-orchestration` teammate-to-teammate |

The runtime never assumes which interface a message came from. It does carry an
`Origin` tag in the message envelope (`origin::cli`, `origin::channel::qq`,
`origin::automation::heartbeat`, …) for observability and policy decisions only.

## A Platform Is More Than An Agent Loop

The lesson from `orangutan/` is that the *interesting* features live in the platform:

- **Orchestration** turns one agent into a team.
- **Automation** turns one prompt into a long-running process.
- **Memory tiers** turn one conversation into accumulating expertise.
- **Hooks** turn the runtime into a place users extend without forking.
- **Permissions** turn "the agent might do anything" into "the agent does only what's
  allowed".
- **Channels** turn the runtime into a multi-platform presence.

The v2 design promotes each of these from "feature" to "first-class subsystem with its
own library, its own tests, its own bench, its own design doc."

## Cross-Cutting Concerns

These six concerns appear in every subsystem and must be designed *uniformly*:

### 1. Observability

- Every action publishes a structured log event with `agent_id`, `runtime_id`,
  `origin`, `cause_event_id`, `latency_ms`.
- Causal IDs propagate across subsystems so a tool call triggered by a channel message
  by an automation firing by a heartbeat tick can be traced end-to-end.

### 2. Cancellation

- Every coroutine takes an `asio::cancellation_slot` or is spawned on one.
- Each subsystem documents what "cancel" means for it (drop in-flight HTTP? finish the
  iteration then exit? abort hard?).

### 3. Backpressure

- Bounded queues are the default; unbounded queues require justification.
- The orchestration mailbox is bounded and `try_send` returns a typed
  `MailboxOverflowed` error.

### 4. Identity

- Every entity has a stable `agent_key` (config-defined) and a derived `runtime_key`
  (per-process scope). Memory and audit logs are namespaced by `runtime_key`.

### 5. Permissions

- Every effectful action passes through the same `oran-permission` rule engine before
  it runs. Tools, file IO, network egress, subprocess exec, and outbound channel
  messages are gated.

### 6. Hooks

- Every lifecycle point is enumerated in
  `docs/design-docs/permissions-and-hooks.md`. New points are added by editing that
  doc, then wiring code to publish them.

## Goals For The First 12 Months

In rough priority order:

1. **MVP runtime** — single agent, CLI REPL, Anthropic + OpenAI providers, file/shell
   tools, session persistence, memory long-term tier, hook bus skeleton.
   See [`../product-specs/0001-core-react-loop.md`](../product-specs/0001-core-react-loop.md).
2. **Channel abstraction + 2 adapters** — QQ (carried over) and Discord (new). Slack
   and Telegram are stretch.
3. **Team collaboration v1** — leader/worker strategy with mailbox, shared scratchpad
   memory tier.
4. **Hook sinks v1** — shell + in-process C++ sinks. Lua sink is stretch.
5. **Permission engine v1** — runtime patterns, replay-signed approval prompts.
6. **Automation engine v1** — cron + periodic + triggered jobs, per-agent leases.
7. **Web UI v1** — single-page chat, session list, admin panel, SSE streaming.
8. **Bench harness v1** — `bench/<lib>/` per library with comparison runner.
9. **Skills v1** — markdown skill loader, skill catalog in the web UI.
10. **Provider portability v2** — Gemini adapter, custom OpenAI-compatible endpoint.

Stretch (12–24 months):

- **Lua hook sink** + sandboxed Wasm sink.
- **Vector memory backend** for long-term tier.
- **Approvals via channel** (e.g., bot pings a Slack channel for human approval).
- **Multi-tenant runtime** (per-tenant identity, isolated memory + audit DBs).
- **Federated agent network** — multiple runtimes coordinate via a typed RPC trait.

## Beyond v2 — Sketches Worth Pursuing

These are not commitments but candidates for `docs/exec-plans/` once core lands.

### Programmable Coordination Strategies

The legacy code's orchestration was hard-coded around `OrchestrationManager`. v2 makes
coordination **a strategy object** — a small interface that picks the next agent to
speak / act given the current conversation graph. Built-ins: leader-worker, pipeline,
voting, free-form. New strategies are added as plugin-like classes registered with
`oran-orchestration::StrategyRegistry`.

### Conversation DAG As First-Class

Multi-agent runs accumulate a directed graph of who spawned whom and who messaged whom.
v2 stores this DAG (in `orchestration.db`) so post-hoc replay, auditing, and learning
loops can ask structural questions ("what fraction of a research task's tool calls were
from the worker vs. the leader?"). The DAG is also a debugging surface — the web UI
renders it.

### Skill Hot-Reload

Skills come from `<workspace>/.orangutan/skills/`. The runtime watches that directory
(asio + inotify on Linux) and re-renders the catalog without restart.

### Provider Cost Awareness

Each `ProviderRoute` declares cost-per-1M-tokens; `oran-provider::execution` accumulates
spend per `agent_key` and emits a hook event when crossing thresholds. Built-in
thresholds + budget-exhausted fallback to a cheaper model.

### Tool Capability Discovery

Tools declare not just "I exist" but "I require: network egress, subprocess exec, file
write". The permission engine consumes that capability list when deciding rule
applicability. New tools cannot accidentally smuggle in undeclared capabilities.

### Channel Bidirectional Threading

Some channels (Slack, Discord) support threads. A v2-native "thread" is a sub-session
inside a `Channel` — the agent's session store can branch into thread sessions and
re-merge.

### Self-Reflective Reports

After every long task an agent emits a `task-debrief.md` into `<workspace>/.orangutan/
debriefs/`. The next task can read recent debriefs as part of working memory. This is
the codebase's path to compounding expertise without retraining.

## Anti-Goals

- Not a framework for *any* AI workload — agent runtimes only.
- Not a hosted service. The binary is meant to run on a developer machine, a single
  VM, or a small fleet. Multi-tenant clustering is stretch.
- Not an LLM router for production-scale traffic. Per-agent throughput is bounded by
  upstream provider rate limits; the runtime does not aim to be a high-QPS proxy.
- Not a substitute for `Claude Code` / `Codex` themselves — Orangutan v2 *uses* them as
  development tools, and *is* the same kind of tool for its operators.

## Where To Go Next

- Concrete features: `docs/product-specs/`.
- Runtime mechanics: `docs/design-docs/{async-model, tool-runtime, memory-system,
  channel-abstraction, team-collaboration}.md`.
- Build-time mechanics: `docs/BUILD_SYSTEM.md`, `docs/FAST_COMPILATION.md`.
- Rule trail: `docs/rules/`.
