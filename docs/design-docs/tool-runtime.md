# Tool Execution

`oran-tool` owns tool definitions, JSON validation, authorization and handlers.
A registered capability is an advertised requirement, never a grant. All effects
from model tool calls pass through `Registry::dispatch`.

## Dispatch

1. Check the model turn's explicit tool selection and resolve the registered
   definition. Registration validates its schema and preparation callback.
2. Apply the blocking `tool_before` hook once. Veto stops the operation;
   rewritten input becomes the input for every following stage.
3. Prepare the call once from final input. A preparer validates arguments and
   returns an owned executor with optional path intent; it performs no IO.
   Invalid filesystem arguments return `invalid_argument`, audit a denial with
   `reason=invalid_tool_input` and finish error/completion observations without
   waiting for a path, resolving authority or spending approval.
4. When `path_locks` is supplied, acquire shared read or exclusive mutation access
   using the declared capabilities and the prepared path's lexical workspace key.
   Vetoed calls never wait for path resources. Cancelled waits finish observations
   without resolving authority, consuming approval or running an executor.
5. Resolve prepared filesystem intent into pinned authority after acquiring the
   lock, so a queued call observes the preceding operation's completed state.
   A displayed path or an earlier path check cannot authorize a later effect.
6. For permissioned tools, evaluate rules for the concrete tool, input, declared
   capabilities and caller. Denial stops execution; ask requires valid approval.
   Runtime tools use their own scope, enablement and admission checks instead.
7. Await the durable decision before invoking the prepared executor at most once.
   Storage failure or cancellation stops execution. Enforce output limits and
   publish the completion observation before releasing the lock. The audit row
   is not rewritten after the effect.

`Registry::add_prepared` registers this pure preparation boundary. Each filesystem
built-in owns its typed arguments and projects path intent from them. Dispatch
retains the prepared value through execution and cleanup. Approval and hooks use
the exact final input bytes, without JSON reserialization.

Trusted registration explicitly sets `DispatchPolicy::runtime` for MemoryRecall,
MemoryRemember, MemoryForget, AgentRun, TaskGet, TaskCancel and host-enabled
[team tools](agent-teams.md#tools-and-task-state). These operations
ignore generic allow/deny/ask, including inherited rules and strict mode. Their
handlers enforce host-bound scope, service availability, memory write gates,
dynamic child identities and task limits. Runtime registrations reject external
capability declarations and filesystem path intent. Hooks can validate/rewrite
or veto requests, but requesting generic approval for a runtime tool is rejected
without opening an approval prompt. Audit admission uses reason `runtime_tool`.
An ordinary custom tool with no capabilities still uses permissioned dispatch;
empty capabilities are not an implicit exemption.

`Registry::add` adapts ordinary handlers into the same dispatch path. It preserves
the registered handler's state and defers concrete argument validation to that
handler. Filesystem-capable ordinary handlers with a string `path` receive
capability-based exclusion and own their authority handling. Names never select
built-in parsing or authority; a custom prepared tool can declare a target from
its own argument shape.

`DispatchContext` carries identity, rules, audit, workspace, approval and injected
memory services. A tool receives these dependencies explicitly. Application
code supplies bindings once instead of registering alternate dispatch paths.
`register_builtins` installs the three filesystem tools;
`register_memory_tools` adds the three memory tools when the host supplies their
services. `register_agent_run` adds a dynamic child runner supplied by the host. A session advertises tools available through its bindings.

## Scheduler

`agent::ToolScheduler` provides bounded parallel calls, ordered results and
per-call timeouts. It owns an opaque `tool::PathLocks` resource and binds it to
each dispatch; it does not parse input or classify filesystem capabilities.
Batches and sessions using the same scheduler share exclusion on its coordinating
strand. Direct registry callers can supply a shared resource on their strand.
The owner retains it until all borrowing dispatches finish.

Read-only calls may share a path lock; mutations exclude other accesses to the
same workspace lock key. Keys normalize final input lexically against configured
roots without filesystem access; they are not inode or symlink-alias locks.
Calls without a lockable path retain their registered dispatch policy.
Path intent does not grant any capability.

Per-call timeout starts after the batch semaphore admits the call and includes
hooks, path waiting, authorization and execution. Path waiting counts toward
approval expiry; dispatch advances the caller's clock sample by the monotonic
wait duration before checking the grant.

The lock table retains only live holders, queued waiters and granted handoffs.
The final participant releases the entry synchronously. FIFO handoff reserves a
permit before posting the waiter's completion; a queued writer prevents later
readers from bypassing it. Cancellation returns any reserved permit and advances
the remaining waiters. There is no idle TTL or host-driven cleanup.

Cancellation has a 100 ms batch grace window. A lagging operation is recorded and
may still be alive after the batch returns. An owner must await
`wait_idle(context)` before releasing that context; `wait_idle()` joins all calls.
This lifetime obligation also applies to borrowed audit, workspace and handlers.
Calls retain the scheduler's lock state through their cleanup, including after a
cancelled batch returns.

## Results And Tool Selection

`Output` separates bounded model-visible text from structured JSON, attachments,
usage and error status. Empty, invalid or oversized output is handled by the
owning output contract. Provider/tool loops preserve tool-call IDs so each result
matches the call that produced it.

`ToolDef` contains a name, description, input schema and required capabilities.
`select_tools` takes a catalogue snapshot and an optional list of names, without
configuration or services. Absence selects all definitions, including custom
tools; an empty list selects none. Explicit names must exist. Repeated requested
names produce one declaration, ambiguous catalogue entries fail, and the result
owns its definitions sorted by name.

The loop selects once per turn and sends descriptions and schemas through native
provider declarations. There is no deferred index, discovery tool or promotion
state. The prompt renderer only fingerprints the selected definitions. Hosts
that previously listed ToolSearch must remove that name; the registration and
promotion APIs and the `ToolDef::deferred` and `ToolDef::category` fields have
been removed.

Explicit selection controls both model exposure and model-call availability.
Dispatch rejects a name omitted from that turn's selected list; snapshots retain
the selection until cleanup. Direct host operations, such as automatic memory
orientation or completion preview reads, use their bound runtime ports without
a model selection. Selection never grants an external capability.
[Prompt design](../rules/prompt-design.md) owns cache identity.

## File And Memory Tools

FileRead, FileWrite and FileEdit are the built-in filesystem tools. Reads use read
authority; writes and edits use mutation authority and conflict checks. Write and
edit must not act on a target replaced after authorization. Exact filesystem
semantics live in [io-runtime](io-runtime.md).

Their preparers validate required fields, field types, known options and positive
integer bounds. Paths must be non-empty and contain
no NUL bytes. Write content limits and invalid edit substitutions are checked
without filesystem access. Unknown fields are rejected as their schemas declare.
File existence, current version, source content and resulting edit size remain
checks after path admission; preparation cannot predict state after a queued writer.
Workspace write intent only carries parent-creation permission. Write modes belong
to the typed write request and the IO operation.

FileRead accepts `path`, optional 1-based `offset` (default 1), `limit` (1–2000,
default 2000), `max_bytes` (1–16777216, default 16777216) and
`allow_outside_workspace` (default false; requires approval). Both absolute and
workspace-relative paths use ordinary admission. Every call reads current bytes.
The model interface exposes line windows only; the IO layer retains byte ranges.
The result starts with path, line span, fingerprint and returned-byte metadata,
followed by exact source text without line-number decoration. The header says
end of file when proven, otherwise gives the next offset. A full window can end
exactly at EOF; the next read then returns an empty body and an EOF hint.
Byte truncation instead requests a smaller limit or larger byte cap: a partial
line must not be skipped by following a guessed continuation offset. Host output
caps can further shorten the result. Files with a single line exceeding the byte
cap cannot be read completely through this model interface.

Native property descriptions own parameter meaning and defaults; tool descriptions
explain when to use the operation and how to recover. Read before changing an
existing file; prefer FileEdit's exact `old_string` / `new_string` replacement to
a complete rewrite. `replace_all` explicitly permits multiple matches. Pass the
read fingerprint as `expected_version` for stale-write protection; on conflict,
read again rather than removing the guard. These tokens are metadata fingerprints,
not content hashes. FileWrite retains explicit append, overwrite and create-only
modes, with overwrite as the default.

The bounded-window and edit guidance follows the collected
[Claude Code Read](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-readfile-compact.md),
[Edit](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-edit.md)
and [Write](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-write.md)
descriptions, and DeepSeek's official
[read renderer](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/fs/tool-fs/src/read-render.ts).
Orangutan keeps its existing tool names and `path` authority contract; it does
not advertise image/PDF reading, shell commands or plugin capabilities it lacks.

MemoryRecall, MemoryRemember and MemoryForget receive a host-bound scope through
injected handlers. They cannot select another scope in tool JSON. Their JSON shape,
bounds, uniqueness and record-kind values are prepared into typed requests before
the effect; handlers perform only the
host-bound memory effect. [memory-system](memory-system.md) owns record and prompt
recall semantics.

## Child Agent Tool

`AgentRun` accepts `{"prompt":"Inspect the change"}` through runtime dispatch.
Only prompt is required. Each call dynamically creates a child; there is no
agent-name selector or preset registry. Extra fields and prompts outside 1–16384
UTF-8 bytes fail before invoking the host binding. Fresh identity/session values
come from the runtime; provider route, workspace, memory scope and external
permissions are inherited from the parent.

The prompt field describes the objective, needed context and paths, constraints,
whether edits are wanted and the expected result. The child does not inherit the
parent conversation automatically; allowed writes affect their shared workspace.
The tool description warns against overlapping writes and asks the parent to
review findings before using them. Each call starts a new conversation, not a
resumption of an earlier child.

The task is prepared before functional admission; the host binding
receives only the typed request and supplies identity, session, memory scope,
provider route and policy. The result text is the child's completed answer.
Structured output contains
`kind=agent_run`, the generated `agent_key` and `session_id`. Output caps,
hooks and audit use the shared dispatch path without generic tool approval. Child
admission exhaustion returns `mailbox_overflowed` with `reason=child_limit` as a
model-visible tool error. Disabled delegation returns `permission_denied`.
[Agent execution](agent-platform.md) owns the host's child-session behavior.

## Native Argument Guidance

Native declarations describe the implemented operation rather than internal
runtime plumbing. File content fields distinguish complete replacement from
append text. Read metadata is not file content, edits use exact strings rather
than patches or regular expressions, and opaque freshness tokens come from a
read of the same file. A conflict requires reading and reconciling current content,
not dropping the guard. FileRead's byte cap bounds source text separately from
the dispatch output cap; FileWrite's cap bounds supplied content, including only
the appended suffix in append mode; FileEdit's cap bounds both complete files.

Schema annotations describe defaults only when they are valid across that field's
uses. MemoryRecall has selector-dependent limits (20 index entries, 5 search hits,
1 exact read), and offset applies only to browsing, so neither field advertises
an unconditional JSON default. Its kinds filter defaults to an empty list.
MemoryRemember advertises importance 0.5 and empty tag/link defaults, and explains
that omitted tags or links clear them when replacing a note. MemoryForget is for
requested removal; successful idempotent removal confirms absence, not prior
existence. The [memory contract](memory-system.md) owns stored-note semantics.

JSON Schema `maxLength` counts characters. The query and child-task descriptions
also state their stricter UTF-8 byte limits (4096 and 16384); preparers enforce
those limits, including multibyte input. Descriptions do not replace validation,
authorization or approval. Schemas keep the existing simple object/property
shape, and changed declaration bytes naturally change native cache identity.

## Capability Vocabulary

The following enum is the stored configuration vocabulary. Some names currently
have no registered handler; they confer no effect by themselves.

```cpp
enum class Capability {
  // file system
  read_file,
  write_file,
  edit_file,
  delete_path,
  list_directory,
  // network
  egress_http,
  egress_websocket,
  // process
  spawn_subprocess,
  signal_subprocess,
  // misc
  external_mcp,
  runtime_loader,
};
```

[permissions-and-hooks](permissions-and-hooks.md) owns rule precedence and
approval binding. [prompt-design](../rules/prompt-design.md) owns cached sections.

## Background Task Tools

A host-bound `BackgroundTasks` service adds `background` (default false) and
`label` (1–120 UTF-8 bytes) to AgentRun's schema. Ordinary AgentRun still awaits
its child. Background mode returns an accepted task receipt after functional admission,
with a service-generated task ID and the actual automatic/query-only delivery
mode. Acceptance is not completion. Hosts without the service keep the original
schema and reject background fields during preparation.

`TaskGet` accepts a returned `task_id`, optional UTF-8 byte
`offset` and `max_bytes` (1–16384, default 8192), and returns immediately. Result
windows are repeatable; an interior code-point offset or a window too small for
one code point is invalid. `next_offset` continues the retained result; separate
retention truncation is explicit. `TaskCancel` accepts only `task_id`. It requests cancellation and reports the current state, including
`cancelling` while cleanup is in progress. Neither tool accepts caller-selected
scope or identity. Foreign, expired and unknown tasks share a not-found response.

Both tools use runtime preparation, owner checks, hooks, durable audit and output
caps. Dispatch snapshots preserve their bound handlers. Children cannot access
these ports or delegate another generation. AgentRun task text is redacted for
untrusted input observers. The [bootstrap contract](bootstrap-runtime.md#background-tasks)
owns task lifetime, retention and completion delivery.

The receipt/query split follows the reference Claude Code
[GetTask description](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-gettask.md)
and [background guidance](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-parameter-bash-run-in-background-guidance.md).
These are third-party prompt extractions, not evidence of Claude Code's private
implementation. [MCP Tasks](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/tasks)
also separates task acceptance, state, results and cancellation; these native
tools do not claim MCP wire compatibility.
