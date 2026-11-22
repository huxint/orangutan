# Runtime Composition

`oran-bootstrap` exposes `RuntimeAssembly`, `HttpProviderBackend` and
`AgentSession` through `<oran/bootstrap.hpp>`. Applications compose these services
from explicit configuration, workspace, executors and session identity.

## Resource Ownership

The host owns the Asio runtime, provider transport, runtime assembly and session.
The session runs on a strand. Blocking filesystem, HTTP and SQLite operations
use the runtime's worker executor. The host awaits the
turn, including audit/trace writes and tool cleanup, before destroying borrowed
services.

`RuntimeAssembly` owns the workspace resolver, approval broker, hook bus, audit
sink and optional session/long-term repositories. Storage paths supplied by the
host select `audit.db`, `sessions.db` and `memory.db`; defaults are under
`<workspace>/.orangutan`. The host owns state-directory privacy and exclusion,
using `io::PrivateDirectory` and its lock when sharing persistent state. Existing
databases use versioned migrations.

`HttpProviderBackend` owns HTTP transport, one provider system and the resolved
route. It passes owned profile values and an explicit credential lookup to
`provider::make_protocol_system`. The [provider contract](api-portability.md)
owns construction validation and per-profile dispatch.

## Configuration Adapters

Bootstrap converts parsed configuration into owned runtime values before
execution. `materialize_permissions` accepts configuration rules, prepends the
mode's baseline and compiles input patterns. Workspace
settings are resolved separately. The returned RuleSet owns its strings and
patterns; materialization retains no rule views. Invalid patterns return an error without
exposing a partial policy. AgentSession supplies the configuration rule view
without copying the surrounding configuration.

`resolve_route_profiles` resolves configured route/profile names, protocol
aliases and model policy into `provider::RouteProfileResolution`. Explicit
protocols take precedence over vendor aliases. This conversion performs no
credential lookup or transport work. Provider construction then validates the
complete route before reading credentials.

These adapters are available through `<oran/bootstrap.hpp>` or their narrow
headers. Provider owns endpoint construction values and `SecretLookup` in
`protocol_transport.hpp`. Config retains parsed values only.

## Session Boundary

`PromptRequest` carries text and optional owned `ImageContent` values. Images
remain typed user blocks through provider requests and persisted history; an
image-only prompt does not introduce an empty text block. Combined encoded image
input is limited to 14 MiB per prompt. Continuation reads 16-MiB forward pages so a
saved image larger than the ordinary browsing page still resumes correctly.

An optional `PromptRequest::parent_turn_id` overrides the session's default trace
parent for that prompt. Reused child sessions supply the turn that initiated each
assignment or correction; this metadata does not alter conversation or cache keys.

`run_prompt` admits one prompt per session on the coordinating strand. A second
in-flight prompt returns `conflict` before effects; the guard covers context
loading, child/tool cleanup and persistence and releases on every exit. This
prevents reentrant hooks or concurrent channel deliveries from corrupting a
shared in-memory conversation or racing checkpoint commits. Independent sessions
continue concurrently.

`AgentSession` loads a working checkpoint and fixed history end, supplies a
bounded forward reader to `agent::Loop`, and appends its original successful
transcript suffix and provisional checkpoint atomically through `Store::append_all`.
The reader hops onto the blocking executor; the loop owns the compacted provider
view. Without storage, the session retains original history and its checkpoint in
memory. Host `AgentSessionOptions::context` supplies the route-wide budget,
summary byte cap and optional input tokenizer; children inherit those options. The session borrows the
provider backend directly; the loop invokes provider execution for retries,
attribution and pricing. The session resolves
`memory.longterm.recall` from configuration and loads the scoped index through
MemoryRecall once before the loop by default. A supplied optional
`longterm_recall` value overrides that policy; exact caller framing replaces
default orientation. The returned index stays stable across model/tool
iterations, and the next prompt observes accepted memory updates. An unavailable
index is explicit context for the model; cancellation still ends the turn.

Memory adapters borrow one backend and capture the host's scope. They
do not capture session state or discover configuration. The session registers
filesystem tools and adds memory tools when memory services
exist. Recall and remember are visible in the default catalogue so the model can
inspect relevant durable notes by ID and save stable decisions or corrections
during ordinary work. The index, content reads and writes share the dispatch
boundary. [Memory](memory-system.md) owns projection, budgets, timestamps and
model-use guidance. Every turn joins its borrowed tool context before returning
or persisting.

Sessions share the broker, workspace, hook and audit services. Each session owns
its rule values. Configuration becomes an optional list of tool names at the
loop boundary; it does not enter prompt rendering or native protocol mapping.
An injected registry exposes custom tools through the same selection function.
The next prompt observes host registration changes. For external tool effects, an `ask` decision fails closed without an approval consumer.
Internal memory and coordination use functional boundaries without generic approval. Embedders may bind an explicit approval sink through the
hook bus.

## Child Sessions

A delegating session registers `AgentRun` when its child budget is nonzero.
Its task prompt supplies the objective and context without selecting a template.
The host creates a fresh `AgentSession`, a random session ID and a generated
`child/<session-id>` agent/approval identity. The child inherits the parent's
provider route, permission mode, active tools and resource bindings, and carries
a borrowed parent-policy view. Parent chat/presentation overlays are cleared;
the task is the child's conversation input. The parent remains alive until
ordinary awaited child calls finish.

Children share the parent's scheduler and strand, use a fresh approval identity,
and return their text plus agent/session identifiers. Child trace rows carry the
parent turn ID. Child token streams do not enter the parent's event sink; the
completed answer returns through the tool result. The child catalogue and any
explicit active-tool selection omit disabled delegation.

Delegation is enabled by tool selection and a nonzero child budget, even with
an empty configuration. AgentRun itself does not evaluate generic permission
rules or ask for approval; children retain inherited external-effect restrictions.
[Agent execution](agent-platform.md) owns admission and lifetime bounds.

## Hosting A Session

1. Parse configuration and create the executors. Use a strand for coordinating
   state and a worker executor for blocking operations.
2. Build `HttpProviderBackend` with the configuration and worker executor, then
   build `RuntimeAssembly` for the workspace and storage paths.
3. Create `AgentSession` with borrowed provider/assembly references, both
   executors, the provider route, scope and agent identities. Supply and retain
   a session ID and agent key for continuation; an all-zero ID starts a fresh
   session.
4. Await `run_prompt(PromptRequest)` on the coordinating strand and consume its
   `Result<PromptResult>`. The return boundary includes tool cleanup and
   successful transcript persistence.

The host supplies optional per-session instructions directly and maps settings for workers, transport limits, hooks, trace and recall into the
corresponding construction options; [configuration](secrets-and-state.md) owns
that mapping. Stream output uses an optional `provider::EventSink`.

Host cancellation propagates through the awaiting coroutine. Tool dispatches and
storage writes finish before their borrowed services are released. The
[HTTP continuation test](../../tests/bootstrap/test_provider_backend.cpp) composes
these interfaces with a controlled transport and a reopened persistent session.

## Channel transport

`inspect_session` reads bounded session/checkpoint metadata and the most recent
trace for an explicit session ID and agent key on the supplied worker executor.
It returns `SessionStatus` values; application formatting is separate. Callers
authorize inspection before calling it. Disabled stores and unavailable trace
records remain distinguishable from an empty session. No transcript bodies or
credentials enter the result, and inspection does not write storage.

`channel_http_transport` binds the SDK-free channel transport port to the host's
HTTP client and current credential lookup. The host maps normalized conversation
keys to retained sessions and owns authenticated ingress and durable delivery.
[Messaging channels](messaging-channels.md) owns protocols, credentials, activity
lifecycle and the composition example.

## Background Tasks

`BackgroundTasks::create` supplies a host-owned registry and scheduler shared by
foreground sessions and dynamic child agents. Bind it with
`AgentSessionOptions::background_tasks`; mismatched executor, configuration,
assembly or injected tool services are rejected. Every call uses the same
coordinating strand. Hosts retain the provider, configuration and assembly until
the service's explicit `shutdown()` closes admission, cancels work and joins it.
Normal parent completion or destruction does not destroy admitted jobs.

Each ordinary task owns a fresh child session, parent permission snapshot, cancellation
group and deadline. Children retain one-generation authority constraints. Defaults
are four running jobs, sixteen queued jobs, sixty-four retained records, a
fifteen-minute deadline including queue time, and 64 KiB of UTF-8 result text.
Host options bound these values. Capacity exhaustion rejects admission; queued
work does not occupy an execution slot. Hosts using blocking HTTP must size
running task admission below worker capacity to keep foreground service available.
Queued cancellation prevents provider work. A running cancellation remains `cancelling`
until the child has completed persistence/tool cleanup and its group has joined.
A deadline reports failure with timeout; it remains active after the start receipt.
Errors expose their category without copying upstream bodies into task results.

`list` supplies scoped status snapshots without model work. `get` pages retained
results without consuming them. Completed records expire after one hour by
default; automatic results awaiting acknowledgment are retained and can fill the
record bound instead of silently disappearing. With session storage enabled,
original child transcripts remain in the existing store. Jobs, claims and
notification acknowledgments are process-local, so no task receipt promises process-restart survival.

`next_completion` exposes a pending terminal task for a host that enabled
`automatic_delivery`. Query-only hosts never promise unsolicited follow-up.
`AgentSession::run_completion(TaskCompletion)` is a typed runtime entry point:
it coalesces up to four ready reports from the same owner, resolves each through
a scoped TaskGet with a 2 KiB preview, frames the reports as
untrusted runtime evidence, and uses the ordinary turn/persistence boundary.
The provider receives a dynamically framed user-role data message, not privileged
system text or an orphan tool result. Its trace origin is `background_task`;
the event is not a user instruction or approval. A busy session rejects a
concurrent completion before claiming it, allowing the host to keep it queued.

A failed continuation releases its claims and leaves the tasks pending. Successful
transcript persistence acknowledges the batch; repeated acknowledged events make no
provider call and return no new reply. Reads never acknowledge delivery. External
message delivery is a separate host responsibility and must retain an answer
before sending it. Failed parent-turn effects still follow the existing turn
contract; a completion retry never reruns the original child automatically.

Explicit task cancellation suppresses its pending automatic notification while
preserving inspection. `cancel_owner` can target a parent session or one originating
turn. Parent cancellation targets tasks started by that turn. Host shutdown
cancels all jobs; it must finish before runtime executors or borrowed services stop.

Separating execution, result retention and notification settlement follows the
same ownership concerns as OpenClaw's
[process registry](https://github.com/openclaw/openclaw/blob/main/src/agents/bash-process-registry.ts)
and [subagent handoff](https://github.com/openclaw/openclaw/blob/main/docs/tools/subagents/slash-command.md#spawn-behavior).
Here the executor is a dynamically created child session, and completion cannot release
borrowed runtime resources before the existing TaskGroup join.

With `BackgroundTaskOptions::enable_teams`, the same service retains named member
sessions across assignments and corrections. Jobs share its ordinary queue,
deadlines, completion settlement and shutdown boundary. No second background
executor is introduced. [Agent teams](agent-teams.md) owns dynamic membership,
reviewed dependencies, direct messages and resource bounds.
