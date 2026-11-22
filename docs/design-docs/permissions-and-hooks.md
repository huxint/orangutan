# Permissions And Hooks

The policy core computes a decision from explicit rules, mode, tool, input and
capabilities. It does not prompt, perform IO or execute the tool. Dispatch owns
those effects after the decision.

This generic policy governs external tool effects. Internal memory, task state
and dynamic child coordination use explicit runtime registration instead.
Their boundaries are host-selected availability, scope, validation and resource
limits, with memory-specific write gates. They never evaluate allow/deny/ask,
even in strict mode. An internal operation grants no authority for an external
operation attempted by a child.

## Decisions

`RuleSet` is an owned sequence of rule values; dispatch borrows a const span.
`permission::evaluate` is read-only. Matching uses a tool-name glob, optional
capability scope and optional RE2 input expression. For each required capability,
precedence is explicit deny, then allow, then ask; the mode supplies the unmatched
default. Within a verdict, the first matching rule supplies its reason and
approval policy. An unscoped rule applies to the whole permissioned tool.

Every required capability must be authorized. Combine capability decisions as
deny, then ask, then allow; a read grant cannot authorize a tool's write effect.
Combined asks use the smallest replay budget and shortest lifetime. A
permissioned tool with no declared capabilities still requires an unscoped rule
or the mode default.

Strict and sandboxed modes deny unmatched effects. Default mode asks for unmatched
effects and installs read-side allow rules. Permissive mode allows unmatched
effects. Configured deny rules still apply. Bootstrap's
`materialize_permissions` compiles the selected baseline, global rule values and
agent overlay once before execution. The permission library consumes the owned
`RuleSet` without importing configuration. Workspace settings remain outside rule
compilation. [Runtime composition](bootstrap-runtime.md) owns the adapter API.

A child dispatch carries an immutable borrowed `PolicyView` for its parent.
Dispatch evaluates both policies independently against the concrete tool, final
input and every required capability, then applies `permission::intersect`.
Deny dominates ask, which dominates allow. When both decisions ask, the replay
budget and approval lifetime use their respective minima. An allow in one policy
cannot satisfy an unmatched or denied requirement in the other. Scheduler context
snapshots retain this parent policy, so rewritten external effects obey the
same restriction. Automatic recall uses the inherited memory scope instead.

`AgentRun` creates a fresh child from its task, with functional enablement and admission bounds.
The host assigns a fresh child approval identity and does not forward parent grants; any child approval binds that child
and the exact final input.

## Approvals

An ask decision requires an explicit trusted consumer or previously issued grant.
With no consumer, dispatch returns `permission_denied` with `approval_required`.
The host supplies an approval consumer through the blocking
`permission_ask_rendered` hook.

The broker authenticates grants with a process-owned key. A grant binds tool,
identity, input hash, expiry and replay budget; default rule policy is eight
replays within one hour. Expired, mismatched, tampered or exhausted grants are
rejected. Restart invalidates process grants. A hook rewrite triggers a fresh
check of the final operation. Time spent waiting for a path lock counts toward
grant expiry; a cancelled wait does not consume replay budget.

Workspace authorization is independent of a tool-name allow. Resolve the target
before approval and carry the pinned filesystem authority through the effect.
Audit writes one durable row per dispatch with the final decision before any
effect; results and usage travel through `tool_after` and the turn trace. Raw
secrets do not belong in either.
Filesystem argument preparation precedes both authority resolution and approval.
Invalid arguments audit a denial with `reason=invalid_tool_input`; they neither
publish an approval request nor consume an existing grant. Validation uses the
final hook input, so a hook may repair an initially malformed request.

## Hook Contracts

A `hook::Sink` is an owned value: an id, an optional `observe` callback, an
optional `decide` callback and a trust flag. `Bus::subscribe` takes ownership;
there is no sink subclassing, unsubscription or publish outcome.

Gates decide before the effect. The bus asks each `decide` callback in
subscription order and the first non-`proceed` decision wins. An error or
exception becomes a `hook_error` veto; exceeding the blocking timeout becomes
`hook_timeout`. The approval prompt is the exception: it waits for the human's
decision until the turn is cancelled. Tool gates may proceed, veto, rewrite the
input or require approval; the memory write gate accepts only proceed or veto.
The decision trace names every consulted gate and travels to the audit row.

Observers run concurrently, cannot change the result and cannot affect the
publisher or sibling sinks. The bus joins them, or abandons one that ignores
cancellation at the advisory deadline; an abandoned observer keeps its sink
alive until it finishes. Untrusted sinks receive redacted inputs, structured
tool output and memory text; trusted local sinks may inspect originals.

- **Blocking**: `tool_before`, `permission_ask_rendered`, `memory_write_before`.

`channel_action` observes channel admission and HTTP authorization before the
effect, with credential-free platform/account/operation metadata. The
[messaging contract](messaging-channels.md) owns those operations and policies.

The event vocabulary follows the production publishers: the agent loop owns
provider observations, registry dispatch owns tool observations, memory bindings
own record observations, and approval resolution owns the prompt gate. Each fact
has one event: `tool_after` reports success or failure, and `provider_response`
names the served target, so a fallback is a served profile that differs from the
route's primary profile. `hook::is_gate` defines the blocking set used by
`publish_blocking<E>`; all other events use advisory publication.

```cpp
enum class Event {
  channel_action,
  provider_request,
  provider_response,
  provider_error,
  tool_before,
  tool_after,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  permission_ask_rendered,
};
```

[tool-runtime](tool-runtime.md) owns dispatch order;
[async-model](async-model.md) owns cancellation and borrowed state.

## Coordination And Task Ownership

AgentRun, TaskGet and TaskCancel are runtime operations without generic tool
approval. Background execution owns a copy of the parent's rules (recompiling
regex values) and creates the child's own rules before returning the receipt.
Child external effects still intersect these policies after hook rewrites, even
after the parent session is destroyed. Approval grants and parent event sinks
are not forwarded.

The service matches parent session ID, scope, agent and identity for every task
lookup/control operation. Possession of an ID never grants access. Host task
commands follow authenticated channel admission and publish `channel_action`;
model calls retain validation, audit and hook observations. Team messaging, skill
activation and scheduling have no current runtime tools; future implementations
must define their concrete functional boundaries.
External messages to people remain subject to channel/transport authorization.

This separation is Orangutan's product contract, not a claim that every agent
uses it. Claude Code's [subagent documentation](https://code.claude.com/docs/en/sub-agents)
allows disabling Agent through permission rules, and its
[permission reference](https://code.claude.com/docs/en/permissions) documents Agent
parameter rules. Its [auto memory](https://code.claude.com/docs/en/memory#auto-memory)
is enabled and scoped as a feature. OpenCode's
[task implementation](https://github.com/anomalyco/opencode/blob/dev/packages/opencode/src/tool/task.ts)
consults `ctx.ask`, while its
[default agent policy](https://github.com/anomalyco/opencode/blob/dev/packages/opencode/src/agent/agent.ts)
defaults to allow. Configurability is distinct from prompting for every internal
operation; Orangutan deliberately uses functional controls for these operations.
