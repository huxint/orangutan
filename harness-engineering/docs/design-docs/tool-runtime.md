# Tool Runtime

The tool registry is the agent's hand. It is the place where:

- The agent decides what it *can* do (the catalog presented to the LLM).
- The runtime checks whether it *may* do something (permissions).
- The runtime observes when it does (hooks).
- The runtime knows when it's *deferred* (tool-search style discovery).

This doc covers the v2 design. The legacy `ToolRuntimeContext` sprawl is explicitly
replaced.

## Tool Definition

```cpp
// include/oran/core/tool.hpp — PUBLIC
namespace orangutan::core {

struct ToolDef {
  std::string                 name;        // dotted: "file.read", "shell.exec"
  std::string                 description; // for the LLM
  nlohmann::json_fwd          input_schema; // JSON Schema for tool args
  std::vector<Capability>     requires;     // what permissions it needs
  bool                        deferred = false; // surfaces via ToolSearch only
  std::optional<std::string>  category;     // for grouping in UIs
};

}  // namespace orangutan::core
```

`Capability` is the v2 mechanism that ties tools to permissions. Examples:

```cpp
enum class Capability {
  // file system
  read_file, write_file, edit_file, delete_path,
  // network
  egress_http, egress_websocket,
  // process
  spawn_subprocess, signal_subprocess,
  // memory
  read_memory, write_memory,
  // orchestration
  spawn_agent, send_message_intra_team, send_message_inter_team,
  // automation
  schedule_job, modify_job, run_job_now,
  // skills
  invoke_skill,
  // misc
  external_mcp, runtime_loader,
};
```

A tool's `requires` list is **inspected at registration**. The permission engine knows
the universe of capabilities a tool might use; the tool cannot smuggle in a capability
it didn't declare (enforced in `ToolRuntime`'s tool dispatch — see below).

## ToolRuntime — Per-Invocation Context

The legacy `ToolRuntimeContext` was a parameter object with 8+ pointer fields. v2
replaces it with a small typed handle that exposes capability-gated services:

```cpp
// include/oran/tool/runtime.hpp
namespace orangutan::tool {

class Runtime {
 public:
  // The dispatching code constructs this; tool code consumes it.
  Runtime(Services&, Capabilities granted, Identity);

  // Capability-gated accessors return Result<T>: forbidden -> error.
  core::Result<io::Workspace&>             workspace();
  core::Result<memory::Runtime&>           memory();
  core::Result<orchestration::Mailbox&>    mailbox();
  core::Result<automation::Runtime&>       automation();
  core::Result<skill::Loader&>             skills();
  core::Result<provider::System&>          provider();
  core::Result<http::Client&>              http();

  // Identity passthrough for logging/audit.
  const Identity& identity() const noexcept;

  // The granted capability set (post-permission).
  Capabilities granted() const noexcept;
};

}  // namespace orangutan::tool
```

If a tool calls `runtime.workspace()` but its `requires` did not include
`Capability::read_file`, the call returns an `Error::capability_not_granted`. This is
defensive against tools that *try* to escalate after registration.

`Services` is owned by `oran-bootstrap`; the `Runtime` holds a reference. No globals.

## Tool Handler Shape

```cpp
namespace orangutan::tool {

struct Output {
  std::string text;                          // primary output
  std::optional<nlohmann::json> structured;  // for the LLM if the protocol supports it
  std::vector<Attachment> attachments;       // files, images, etc.
  std::optional<double> cost_estimate;       // for cost-aware planning
  bool is_error = false;
};

using Handler =
    std::function<async::Awaitable<core::Result<Output>>(
        const nlohmann::json& input, Runtime&)>;

}  // namespace orangutan::tool
```

Tools are coroutines. They can `co_await` other services. They run on the agent's
strand by default; for CPU-heavy tools, `co_await async::post(runtime.cpu_executor())`.

## Registry

```cpp
namespace orangutan::tool {

class Registry {
 public:
  // Registration.
  core::Result<void> add(core::ToolDef def, Handler handler);
  core::Result<void> remove(std::string_view name);

  // Discovery.
  std::vector<core::ToolDef> catalog() const;
  std::vector<core::ToolDef> deferred_catalog() const;
  std::optional<core::ToolDef> find(std::string_view name) const;

  // Dispatch (called by the agent loop).
  async::Awaitable<core::Result<Output>>
  dispatch(std::string_view name,
           const nlohmann::json& input,
           permission::Evaluator& perms,
           hook::Bus& hooks,
           Identity identity) const;
};

}  // namespace orangutan::tool
```

Note that `dispatch` *takes* the permission evaluator and hook bus by reference. The
registry does **not** own them; bootstrap does.

## Built-in Tool Categories

Each category is a small library that calls `Registry::add` from a single
`register.cpp`:

| Library                       | Tools                                                   |
| ----------------------------- | ------------------------------------------------------- |
| `oran-tool-file`              | `file.read`, `file.write`, `file.edit`, `file.search`   |
| `oran-tool-shell`             | `shell.exec`, `shell.glob`, `shell.ls`, `shell.move`    |
| `oran-tool-memory`            | `memory.recall`, `memory.remember`, `memory.forget`     |
| `oran-tool-orchestration`     | `agent.spawn`, `agent.stop`, `agent.send_message`, …    |
| `oran-tool-automation`        | `automation.schedule`, `automation.list`, …             |
| `oran-tool-mcp`               | external MCP client (one tool per external MCP server)  |
| `oran-tool-skill`             | `skill.invoke`                                          |
| `oran-tool-background`        | async job orchestration                                  |
| `oran-tool-attachments`       | message attachment management                           |
| `oran-tool-runtime-loader`    | dynamic tool reloading                                  |
| `oran-tool-search`            | deferred tool discovery / metadata lookup               |
| `oran-tool-script`            | scriptable batch tool                                   |

Each tool library is **independent** — adding a new category does not recompile the
others.

## File vs. Shell — De-duplicated

The legacy code's 1.5 kLoC of file tools + 0.7 kLoC of shell tools had overlapping
glob / ls / mkdir / move logic. v2 split:

- **`oran-tool-shell`** owns process execution, glob/ls/mkdir/mv as the *raw* surface
  with subprocess semantics and tty-style output.
- **`oran-tool-file`** owns **structured** operations: `file.read` returns content +
  line numbers + checksum; `file.edit` performs patch-style edits with conflict
  detection; `file.search` returns ripgrep-style structured matches.

Operations that exist in both libraries (`glob`, `ls`, `mkdir`, `move`) are **only**
in `oran-tool-shell`. `oran-tool-file::file.search` may call into shell glob
internally, but the public surface is distinct.

## Permission Ordering

The dispatch order is fixed:

```
1. Hook bus → tool.before     (advisory; may short-circuit on error)
2. Permission engine evaluates rules against {tool name, input, identity, capability set}
3. If "ask" → render approval prompt, wait, replay-sign
4. If "allow" → continue; if "deny" → return PermissionDenied error
5. Hook bus → tool.dispatched (after permission, before handler)
6. Handler runs
7. Hook bus → tool.after       (always, even on error)
```

The legacy code's per-tool boilerplate ("check permission inside the tool handler") is
gone; the registry does it once. Tools that need *additional* fine-grained checks (e.g.
network egress allowlist) call `permission::Evaluator::check_capability(...)` inside
their handler.

## Deferred Tools

Some tools are **deferred** — present in the catalog but not surfaced to the LLM until
explicitly looked up via `tool-search`. This pattern compresses the prompt without
losing capability.

Implementation:

- `ToolDef::deferred = true` registers the tool in `deferred_catalog()` only.
- The default system prompt builder lists the deferred tool *names + one-line descriptions*
  in a compact "Deferred Tools" section.
- `tool-search` is a non-deferred tool that returns the full schema on demand.
- The registry keeps a per-agent set of "promoted" deferred tools whose full schema is
  now in the prompt; the prompt builder honors it.

`async::Channel<Promotion>` could push promotions across iterations if needed; for now
a per-loop mutable set is simpler.

## Output Scrubbing

Tool output may contain secrets (e.g. environment variables echoed by a shell tool).
v2 routes every tool output through `oran-log::redact` which uses **runtime** (not
compile-time) regex patterns loaded from `config.runtime.redaction_patterns`.

The legacy code used `ctre` (compile-time regex). v2 uses `re2` (small TU, fast,
runtime-configurable). See `docs/rules/libraries.md`.

## Hook Surface

Tool lifecycle hooks:

- `tool.before(name, input, identity)` — may rewrite input or short-circuit.
- `tool.dispatched(name, input, identity)` — after permission, before handler.
- `tool.after(name, input, output, identity, duration)` — always.
- `tool.error(name, input, error, identity)` — if handler returned an error.

See `permissions-and-hooks.md` for sink kinds.

## Anti-Patterns

- Tools that reach into globals (`workspace_root` via a singleton). They take the
  `Runtime` handle.
- Tools that spawn background coroutines without registering them with the runtime's
  cancellation token.
- Tools that ignore the `Capability` declaration and use a service anyway.
- Tools that call `provider::System::send` themselves to "ask another model". That's
  agent territory; use `agent.spawn` or the orchestration tools.

## Bench

`bench/oran-tool/` ships:

- `bench/oran-tool/registry_lookup` — N-tool catalog, repeated `find()` calls.
- `bench/oran-tool/dispatch_overhead` — dispatch path latency without doing real work.
- `bench/oran-tool/permission_eval` — cost of permission engine on realistic rule sets.

`compare.cpp` reports the dispatch overhead as a percentage of typical tool latency
(file.read of 4 KB; shell.exec of `/bin/true`).
