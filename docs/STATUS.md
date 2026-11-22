# Current State

Orangutan runs a C++26 provider/tool/session loop through library composition.
The implemented core includes scoped memory, persisted continuation, explicit
authorization and bounded child collaboration.
[Architecture](ARCHITECTURE.md) owns the library graph.

## Runtime Boundary

Bootstrap converts explicit configuration into owned permission rules and provider
profiles, then assembles workspace, audit, hooks and optional memory services.
Config is consumed at composition; prompt rendering depends only on core values.

Each turn owns one selected native tool catalogue and one joined system prefix.
Conversation remains typed. Cache identity follows submitted text and native
declarations, without manually maintained section versions or old-key guarantees.

Provider sends target one configured endpoint. Execution owns retry/fallback
selection, terminal attribution and cost estimation. The loop consumes those
outcomes for hooks and traces and accumulates usage. Backend error strings do not
choose the attributed profile.

FileRead, FileWrite and FileEdit use prepared, validated calls and pinned
filesystem authority. Memory tools and AgentRun share validated dispatch but use
internal scope/admission rules rather than generic allow/deny/ask.
Unknown tool selections fail before provider work; visibility grants no authority.
Path locks follow live holders and waiters, and cancellation joins borrowed work.

Sessions load a working checkpoint, forward history pages and a scoped memory index.
The provider view compacts older complete exchanges under context pressure;
original transcript suffixes and checkpoint coverage commit together. Exact reads, lexical
search and same-turn note corrections run through memory tools. Successful
transcript suffixes serialize before acquiring the writer and commit atomically.
Browsing does not update read timestamps; corrections preserve record history.

Dynamic children receive fresh agent/session/approval identities, task-specific prompt
and tool context, and the intersection of parent/child permissions. They share
workspace, memory scope, provider route, scheduler and strand. Admission defaults
to four children per prompt and one generation; parent cancellation joins them
without waiting for unrelated sessions.

Hosts may bind bounded background child tasks with scoped status/result reads
and cancellation. Their owned authority and shared tool services survive parent
turns; terminal state follows joined cleanup. Typed completion continuations
coalesce ready reports and acknowledge only after parent persistence. Jobs are
process-local; child transcripts remain stored. The Telegram host supplies
completion polling, delivery journaling and `/tasks`/`/stop` controls.

An enabled background service also provides dynamic agent teams: the leader
creates and adds roles, reuses specialist sessions, reviews dependency results
and requests bounded corrections. Peers exchange targeted messages at model
boundaries without changing task authority or waking idle members. The retained
Telegram host enables the tools. [Agent teams](design-docs/agent-teams.md) owns
the current controls and the limits of cache and cost claims.

Filesystem, HTTP and SQLite work runs on explicit executors. Durable audit
decisions precede tool effects; terminal traces and cancellation cleanup finish
before services are released. Existing user records remain intact.

QQ, Telegram and Feishu adapters expose injected transport and turn ports.
The channel dispatcher bounds admission/deduplication, retains failed deliveries
and joins typing-status cleanup. Hosts own authenticated ingress and durable
outboxes; [messaging channels](design-docs/messaging-channels.md) owns the contract.
The opt-in `oran-telegram` host supplies authenticated polling for one allowed
private user, text and image input, persistent session routing, a durable
intake/delivery journal and explicit reconciliation after ambiguous failures.
Bootstrap also supplies official QQ webhook authentication, durable-enqueue ACKs
and scoped token refresh. Public QQ ingress, queue/outbox deployment and account
configuration remain host responsibilities in the messaging contract.
The opt-in `oran-qq-login` obtains credentials by official QR authorization,
stores the binding privately without overwriting existing accounts, and probes
saved credentials. It does not start QQ message ingress. The opt-in `eval-qq` runner and bounded
Gateway harness exercise owner-only live dialogue, persisted references, images,
native Markdown, typing and local commands through the existing contracts;
[testing](rules/testing-and-bench.md#qq-live-dialogue-evaluation) owns their limits.

## Contract Owners

- [Composition](design-docs/bootstrap-runtime.md): resources and host bindings.
- [Agent](design-docs/agent-platform.md): turns, context and child execution.
- [Provider](design-docs/api-portability.md): attempts, outcomes, streaming and cache policy.
- [Prompt](rules/prompt-design.md): stable text and current fingerprint rules.
- [Tools](design-docs/tool-runtime.md) and [permissions/hooks](design-docs/permissions-and-hooks.md): admission and effects.
- [Memory](design-docs/memory-system.md), [storage](design-docs/storage-runtime.md) and [IO](design-docs/io-runtime.md): durable data and resource ownership.

Development permits breaking APIs, configuration and derived caches. Replace
obsolete surfaces and their tests/docs directly, as defined by
[collaboration](REPO_COLLAB_GUIDE.md); do not maintain parallel historical versions.

## Verification And Next Work

The optional [desktop browser prototype](../apps/desktop/README.md) implements
the [desktop product contract](product-specs/desktop-prototype.md) using simulated
execution and channel events. It includes five pages, scoped state, read-only
channel views, approvals for local work, bilingual themes and page-close recovery.
Its separate frontend checks cover state behavior and browser interactions;
these do not establish live provider/channel or native desktop integration.

The release gate covers all 15 test targets and `make ci`. Controlled HTTP
integration composes transport, assembly and session continuation. Regressions
cover denied effects, atomic persistence, scoped recall, fallback attribution,
cost, stable prompt snapshots and joined cancellation. These checks establish
runtime behavior, not spontaneous memory use or service-side cache hits.

[Live debt](exec-plans/tech-debt-tracker.md) tracks durable-note provenance,
shell execution, real-model memory/context evaluation,
hosted quality jobs and reference-hardware compile budgets. SQLite snapshots and
explicit session and whole-scope memory imports support recovery without rewriting
original records. Memory imports rebuild search and reject occupied destinations.

The opt-in `eval-context` runner compares full-history and compacted sessions
using synthetic retention tasks, reopening and fixed tool output. Controlled
checks cover grading and execution. Live DeepSeek Flash runs exercise both paths;
strict field failures expose task-label paraphrasing and inspection logs entering
task-state fields. These synthetic results do not establish general task success.
[Testing](rules/testing-and-bench.md#working-context-evaluation) owns the runner
contract; [live debt](exec-plans/tech-debt-tracker.md) scopes the follow-up.

Run the normal release gate from the repository root:

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

[Build system](BUILD_SYSTEM.md) owns supported options and toolchains;
[testing](rules/testing-and-bench.md) owns affected targets and sanitizer checks.
