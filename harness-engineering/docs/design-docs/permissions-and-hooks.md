# Permissions And Hooks

The runtime is observable and controllable at well-defined points. **Permissions**
gate effectful actions; **hooks** publish lifecycle events so external code can react.
The legacy code's hook surface was narrow (tool lifecycle + a few message events) and
permissions used compile-time regex (`ctre`) — v2 expands both.

## Permission Engine

### Sources

Rules come from three layers, merged at runtime:

1. **Built-in defaults** (`oran-permission::Defaults`) — safe baseline.
2. **Global config** — `config.permissions`.
3. **Per-agent overlay** — `config.agents.<name>.permissions`.

Later layers override earlier ones; explicit `deny` always wins over `allow`.

### Rule Shape

```cpp
struct Rule {
  Verdict     verdict;        // allow | deny | ask
  std::string tool_pattern;   // glob: "file.*", "shell.exec", "memory.write"
  std::optional<InputPattern> input_pattern;  // optional, runtime regex
  std::optional<Capability>   capability;     // gate by capability
  std::optional<std::string>  reason;         // human-readable in approval prompt
};
```

`InputPattern` is a runtime regex (re2). Examples:

```yaml
allow:
  - file.read
  - file.search
  - "shell.exec(/bin/{ls,cat,head,tail,grep,find}:*)"
deny:
  - "shell.exec(rm:*)"
  - "shell.exec(git push *)"
ask:
  - file.write
  - file.edit
  - "shell.exec"
```

### Modes

A profile selects defaults:

| Mode          | Default for tools not matched by any rule |
| ------------- | ----------------------------------------- |
| `auto`        | allow                                     |
| `default`     | allow read-side; ask for write-side       |
| `permissive`  | allow most; deny only the dangerous       |
| `strict`      | deny by default; allow explicit only       |
| `sandboxed`   | read-side only; deny everything else      |

### Evaluation

`permission::Evaluator::evaluate(tool_name, input, capabilities, identity) -> Verdict`

Algorithm:

1. Resolve effective rule set (defaults + global + per-agent).
2. Apply explicit `deny` rules first; if any match, return `deny`.
3. Apply `allow` rules; first match wins, return `allow`.
4. Apply `ask` rules; first match wins, render approval prompt.
5. Default by mode.

The legacy code's "signed approval prompt" pattern continues. The signature scheme:

- Approval prompt is HMAC-signed with a per-process secret.
- Approval replay is allowed within `approval_ttl` (default 1h) for the same
  `(tool_name, input_hash, identity)` triple.
- TTL and replay count are configurable per rule (`replay_max`, default 8).

### Capability-Aware Gating

A tool's `requires` list (see `tool-runtime.md`) is part of the evaluator's input. A
rule may scope to a capability:

```yaml
allow:
  - "*  capability=read_file"
ask:
  - "*  capability=spawn_subprocess"
deny:
  - "*  capability=runtime_loader"
```

This is more expressive than "match by tool name" and survives tool renames.

### Runtime vs. Compile-Time Regex

Legacy used `ctre`. v2 uses **`re2`** (Google's library). Reasons:

- Patterns are now config-driven (compile-time impossible).
- `re2` has linear time guarantees against pathological input.
- Smaller TU footprint than `ctre`.

`docs/rules/libraries.md` codifies this choice.

## Hook Bus

### Surface

```cpp
// include/oran/hook/bus.hpp — PUBLIC
namespace orangutan::hook {

enum class Event {
  // agent
  agent_start,
  agent_stop,
  iteration_start,
  iteration_end,
  final_response,
  // provider
  provider_request,
  provider_response,
  provider_error,
  provider_fallback,
  // tool
  tool_before,
  tool_dispatched,
  tool_after,
  tool_error,
  // memory
  memory_read_before,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  memory_decay,
  // channel
  channel_start,
  channel_stop,
  channel_inbound,
  channel_outbound_pre,
  channel_outbound_post,
  channel_delivery_error,
  // orchestration
  team_created,
  worker_spawned,
  worker_stopped,
  team_message,
  team_broadcast,
  conversation_completed,
  conversation_aborted,
  // automation
  job_scheduled,
  job_started,
  job_finished,
  job_failed,
  // session
  session_start,
  session_end,
  // permission
  permission_ask_rendered,
  permission_ask_resolved,
  permission_denied,
};

class Bus {
 public:
  // Subscription returns a typed handle whose destruction unsubscribes.
  template <Event E>
  [[nodiscard]] Subscription subscribe(Sink&);
  Subscription subscribe(std::initializer_list<Event>, Sink&);

  // Publish; the bus dispatches to all sinks subscribed to the event.
  async::Awaitable<core::Result<DispatchOutcome>> publish(Event, Payload);
};

}  // namespace orangutan::hook
```

### Sink Kinds

`Sink` is an abstract interface; built-in implementations:

| Sink kind     | When to use                                                  |
| ------------- | ------------------------------------------------------------ |
| `ShellSink`   | External script (the legacy default). Sub-process, JSON on stdin. |
| `InProcessSink` | C++ callback — for code that lives inside the binary itself. |
| `LuaSink`     | (stretch) embedded `sol2` / `luajit` runtime. Hot-reloadable. |
| `WasmSink`    | (stretch) wasmtime; sandboxed.                                |
| `WebhookSink` | HTTP POST to a URL via `oran-http::Client`.                  |

Sinks declare `kind()` and a `Capabilities` struct (e.g. "this sink may block"; the
bus respects blocking-vs-fire-and-forget semantics).

### Synchronous vs. Async Hooks

Each event is annotated `blocking` or `advisory`:

- **Blocking** (e.g., `tool_before`, `memory_write_before`, `permission_ask_rendered`):
  the bus awaits all sinks. A sink may return a `Decision` that vetoes / rewrites /
  proceeds.
- **Advisory** (e.g., `tool_after`, `iteration_end`): the bus fires-and-forgets. Sinks
  cannot veto.

This is statically known per event so the type system can enforce it:

```cpp
template <Event E>
struct EventTraits;

template <>
struct EventTraits<Event::tool_before> { using Decision = ToolBeforeDecision; };

template <>
struct EventTraits<Event::tool_after>  { /* no Decision; advisory */ };
```

### Configuration

```jsonc
{
  "hooks": {
    "sinks": [
      { "id": "shell-1", "kind": "shell", "path": "/.orangutan/hooks/pre-tool.sh" },
      { "id": "audit",   "kind": "webhook", "url": "https://audit.local/event" }
    ],
    "bindings": [
      { "sink": "shell-1", "events": ["tool_before", "tool_after"] },
      { "sink": "audit",   "events": ["permission_denied", "tool_error"] }
    ]
  }
}
```

### Audit

Every blocking hook decision is recorded in `audit.db` with `event`, `sink_id`,
`identity`, `decision`, `latency_ms`. The CLI / web admin can replay an audit log.

### Failure Modes

- A blocking sink that times out (`config.hooks.timeout_ms`, default 2000) → the bus
  treats it as `Error::HookTimeout`. The triggering action is *not* executed; the agent
  receives a `tool.error`-style response.
- A blocking sink that crashes (shell exit ≠ 0) → same as timeout.
- An advisory sink failure → logged at WARN; otherwise ignored.

## Per-Agent Wiring

The bootstrap reads `config.hooks.bindings` once and constructs the bus. Each
agent's `Loop` gets a reference to the same bus. Per-agent subscription filtering can
be done with predicate sinks if needed (rare; usually a sink subscribes to all
agents and filters by `identity` in its payload).

## Hook Surface Discoverability

A meta-tool `hook.events` lists all enumerated events, their `blocking|advisory`
annotation, and their payload shape. Agents can query it to know what they can react
to.

## Anti-Patterns

- Sinks that call back into the agent (calling `provider::System::send` from a
  `tool_after` hook). The blocking-vs-advisory contract is unidirectional; sinks
  observe, they don't drive.
- Hooks used to implement features that should be subsystems. If three different
  consumers want a hook that does the same thing, that's a sign for a real
  subsystem (e.g., cost-tracking should be a subsystem, not a hook).
- Hooks that mutate the agent's working memory directly. The hook payload can carry a
  "suggested-mutation", but applying it is the runtime's job.

## See Also

- [`tool-runtime.md`](tool-runtime.md) — tool dispatch ordering with hooks.
- [`agent-platform.md`](agent-platform.md) — six cross-cutting concerns including
  permissions and hooks.
- [`../product-specs/0008-permissions.md`](../product-specs/0008-permissions.md)
  — concrete v1 deliverables.
