# Architecture

Orangutan is a C++26 agent runtime. Its first useful boundary is a completed turn:
load scoped context, call a provider, execute authorized tools, return tool
results to the provider, and persist the response. The
[agent contract](design-docs/agent-platform.md) also composes bounded child turns
through that boundary; [STATUS.md](STATUS.md) records verification.

## Functional Core And Effects

Policy evaluation, prompt rendering, schema validation and message conversion
operate on explicit values. They do not discover configuration or start work.
Provider, filesystem and SQLite operations are effect boundaries coordinated by
Asio. RAII owns resources; cancellation requests are followed by a lifetime join.

The session coordinator loads a checkpoint and scoped memory index, supplies a
bounded forward history reader, runs a turn and atomically persists its completed
suffix and checkpoint. The loop keeps a compacted provider view separate from the
original messages it returns for persistence. Memory adapters borrow explicit storage services.
Tool selection and pure prompt rendering produce one owned catalogue and stable
system prefix per turn. Native declarations contribute to the fingerprint;
conversation stays in typed messages. New abstractions must express a current
domain boundary with a concrete caller.

```mermaid
flowchart TD
  app[Application composition] --> session[Session and context]
  session --> loop[Agent turn]
  session --> memory[Scoped memory and history]
  loop --> provider[Provider protocol and retry]
  loop --> scheduler[Bounded tool scheduler]
  scheduler --> dispatch[Prepare input and select effect boundary]
  dispatch --> effects[Authorized filesystem effects]
  dispatch --> memory
  dispatch --> children[Bounded child sessions]
  children --> session
  dispatch --> audit[Audit and hooks]
```

## Boundaries

| Library | Responsibility |
| --- | --- |
| `oran-core` | Messages, identifiers, errors, capabilities and value utilities. |
| `oran-async` | Executors, cancellation, bounded queues and owned child work. |
| `oran-io` | Filesystem operations, pinned authority handles and blocking work. |
| `oran-http` | Bounded HTTP/SSE transport. |
| `oran-storage` | SQLite, migrations, pools and repositories. |
| `oran-config` | Parse and validate explicit configuration values. |
| `oran-permission` | Rules, decisions, bounded approval grants and audit sinks. |
| `oran-hook` | Typed effect gates and advisory lifecycle observations. |
| `oran-memory` | Session serialization, scoped notes, a bounded discovery index and lexical recall. |
| `oran-tool` | Tool selection, prepared calls, path admission, authorization and execution. |
| `oran-prompt` | Pure text rendering and cache fingerprints over explicit core values. |
| `oran-provider` | Protocol mapping, credentials, retries, fallback, attribution and pricing. |
| `oran-agent` | Provider/tool turn execution and bounded scheduling. |
| `oran-channel` | Messaging protocol strategies, bounded delivery and joined activity lifecycle. |
| `oran-bootstrap` | Construct resources and drive one session from the application. |

Applications link the runtime libraries and supply configuration, executors and
session identities through `oran-bootstrap`. The host owns process lifetime and
presentation. Tests and benchmarks exercise these interfaces through their own
runners. See [BUILD_SYSTEM.md](BUILD_SYSTEM.md).

The optional [desktop prototype](../apps/desktop/README.md) under `apps/desktop`
is a browser UI with a pure state model, source-restricted commands and injected
demo/storage effects. [Desktop implementation](design-docs/desktop-ui.md) owns
its module map, state/effect boundaries and development handoff. It does not link or call the runtime. Its
[product contract](product-specs/desktop-prototype.md) owns local actions,
read-only channel observation and the future integration boundary.

The opt-in `oran-telegram` application in `apps/telegram/` composes authenticated
polling, one allowed private user's persistent session and a private delivery
journal. Its deployment and recovery contract belongs to
[messaging channels](design-docs/messaging-channels.md#telegram-deployment-host).
The opt-in `oran-qq-login` in `apps/qq/` composes bootstrap's official QR
authorization with terminal display and private credential storage. It binds a
bot or probes its saved credentials; the QQ message receiver remains host work.
The opt-in `eval-qq` runner composes a persisted session and QQ dispatcher for
one trusted local event; [testing](rules/testing-and-bench.md#qq-live-dialogue-evaluation)
owns this bounded live-test surface.

Dependencies flow toward domain values and platform primitives. Intentional
same-layer edges are HTTP/IO/storage → async and tool → permission/hook.
Config depends only on core values; bootstrap is its only runtime-library
consumer and maps configuration into owned rules and provider profiles.
Permission, provider and agent execution have no configuration dependency.
Prompt depends only on core; it has no tool-runtime or executor dependency.
Provider consumes its own stable-prefix values and has no prompt dependency;
the loop connects those value boundaries. `scripts/check-deps.sh` checks the
graph and enforces the configuration boundary. The composition root joins
runtime, memory and transport. Channel depends on core, async, permission and
hook; bootstrap binds its transport to HTTP. Channel has no session dependency.
[Messaging channels](design-docs/messaging-channels.md) owns its host ports.

## Authority And State

Each turn carries session, agent and scope identity. An external tool definition declares
required capabilities; a rule decision grants or refuses the concrete operation.
Internal memory and coordination instead use host-bound scope, functional
availability and admission limits, without generic permission prompts. Filesystem handlers receive pinned authority handles after path
resolution. Approval binds the identity, tool and exact approved input.

Session history and personal records are separate stores. Prompt history reads
are bounded; persisted history remains intact. Session, memory, audit and trace
SQLite calls run on the blocking executor. Hooks and coordinating state run on
the session's strand. Awaited writes finish before borrowed services are released.

`AgentRun` reuses the turn boundary with independent child sessions. Parent and
child policy decisions intersect at dispatch. Children share the parent's
workspace, memory scope, provider route and tool scheduler; fresh approval
identities keep grants separate. Child counts and delegation depth are bounded,
and parent cancellation joins borrowed child work. For background execution,
a host-bound bootstrap service owns child sessions and shared tool services
beyond the parent turn. Scoped completion events resume the parent at a safe
turn boundary; the host joins the service before releasing runtime resources.

The same service optionally retains leader-defined specialists, a reviewed task
graph and bounded peer inboxes. Jobs reuse its queue and scheduler; the loop only
consumes dynamic message values. [Agent teams](design-docs/agent-teams.md) owns
their topology, authority, lifetime and cost/cache strategy.

## Public Headers

```text
<oran/agent.hpp> <oran/async.hpp> <oran/bootstrap.hpp> <oran/channel.hpp> <oran/config.hpp>
<oran/hook.hpp> <oran/http.hpp> <oran/io.hpp> <oran/memory.hpp>
<oran/permission.hpp> <oran/prompt.hpp> <oran/provider.hpp>
<oran/storage.hpp> <oran/tool.hpp>
```

Use a narrow header when it avoids pulling unrelated declarations into a
translation unit. Heavy third-party definitions stay in implementation files.
The [design index](design-docs/index.md) routes detailed contracts.
