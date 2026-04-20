# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Orangutan** is a C++23 agent assistant: a single binary that runs an LLM ReAct loop with a tool registry, pluggable providers, persistent memory/sessions, an HTTP web UI, external chat channels (QQ), a coordinator that spawns worker agents, and a cron-like automation runtime. Act as an expert C++23 developer producing **correct, maintainable, and performant** code that matches the conventions below.

## Build, Test, Lint

The project uses **xmake** (not CMake). Packages are pinned in `xmake-requires.lock`.

```bash
# Configure (first time or when switching modes)
xmake f -m release         # or -m debug

# Build
xmake                      # build all targets (slow — minutes)
xmake build orangutan      # binary only
xmake build orangutan-lib  # static lib only (the shared link target for tests)
xmake build test-agent     # one test bucket only (much faster iteration)

# Run the binary
xmake run orangutan -- --help      # preferred
./orangutan --help                  # symlink → build/linux/x86_64/release/orangutan

# Tests (Catch2)
xmake test                                # run every test-* target
xmake run test-agent                      # run one bucket
xmake run test-agent "[tag]"              # Catch2 tag filter
xmake run test-agent "test case name"     # by test case name
xmake run test-agent -s                   # show successful assertions

# Options
xmake f --qq_channel=n     # disable the QQ channel (compiled in by default)

# Formatting / lint
# A pre-commit hook (.githooks/pre-commit) auto-runs clang-format on staged C/C++ files.
# If not installed: git config core.hooksPath .githooks
# clang-tidy is governed by .clang-tidy; treat its warnings and LSP diagnostics as errors.
# compile_commands.json auto-regenerates on build (configured for clangd).
```

**Test target map.** Each directory under `tests/` is compiled as its own `test-<name>` target — see `xmake/tests.lua`. When iterating on module X, run only `test-<x>` rather than the whole suite. Test helpers live at `tests/test-helpers.hpp` (unique tmp paths, `ScopedEnvVar`) and `tests/test-provider-support.hpp` (`FakeProviderBackend`, `make_test_route`).

**Builds are slow (~70s+).** Batch edits, rely on clangd diagnostics, and build only at the end of a change series. Do not rebuild after every trivial edit.

## Architecture — The Big Picture

`main.cpp` does nothing except call `orangutan::bootstrap::run(argc, argv)`. Everything interesting is in `src/bootstrap/`, which is the **runtime assembler**: it parses CLI flags, loads/decrypts config, constructs per-agent runtimes, wires them into channel/web/automation services, and drives the main loop. Read `src/bootstrap/bootstrap.cpp` first — it names every collaborator.

### Core primitive: the agent loop

`agent::AgentLoop` (src/agent/agent-loop.hpp) is the ReAct loop. Given a user prompt, it iterates up to `MAX_ITERATIONS`: build request → `ProviderSystem::send` → parse tool calls → dispatch via `ToolRegistry` → append results to history → repeat until a final text response or stop. It composes with **providers**, a **tool registry**, optional **runtime memory**, **hooks**, and a **skills** prompt. Configure via the fluent `AgentLoopBuilder` (uses C++23 deducing-`this`). One `AgentLoop` per running agent instance; multiple instances coexist (primary CLI + coordinator workers + automation-triggered runs).

### Providers: HTTP → protocol → execution

`src/providers/` is layered:

- **transport/** — libcurl primitives + SSE parser (`http-transport`, `sse-parser`).
- **protocols/** — per-API adapters (`anthropic-messages`, `openai-chat-completions`, `openai-responses`). `protocol-adapter.hpp` is the common interface; `provider-registry` maps `provider_kind × protocol_kind` to an adapter.
- **execution/** — `runtime-backend` applies retry, fallback-model switching, and aggregates `ProviderUsageStats`.
- `provider.hpp` defines `ProviderSystem`, `ProviderRoute` (primary + fallbacks), `ModelTarget`, and `stdexec`-based sender types (`provider_sender`). Every async edge is a sender/receiver — **do not introduce `std::thread` or custom pools**.

### Tools: registry + dispatch + permissions + hooks

`src/tools/`:

- **registry/** — `ToolRegistry` holds tool definitions and dispatches calls through a `ToolRuntimeContext` (workspace root, permissions, memory, coordinator, mailbox, automation, skill loader, etc.).
- Per-category subdirs register themselves via `tools::register_builtin_*` (see `tools/register.hpp` and each subdir's `register.cpp`): `file/` (read/write/edit/search via fd+rg), `shell/` (sandboxed shell exec), `mcp/` (external MCP clients), `memory/`, `coordinator/` (spawn/stop/send-message to worker agents), `swarm/` (team mgmt), `automation/`, `skill/`, `message-attachments/`, `tool-search/`, `script/`, `runtime-loader/`, `background/`.
- `permissions/` evaluates allow/deny/ask rules (`permission-evaluator`, `rule-parser`, `safety-checks`) and produces approval prompts signed for replay.
- `hooks/HookManager` runs external shell hooks at tool lifecycle events.

Shell already covers `ls/glob/mkdir/delete/move`; new file tools should only exist when they provide structured output, patch semantics, or wrap a real binary — don't duplicate shell.

### Coordinator, swarm, automation

- **coordinator/** — `CoordinatorManager` spawns worker `AgentLoop` runtimes on demand via a factory supplied by `bootstrap` (see `set_worker_runtime_factory`). Max-concurrent spawns is per-agent (`coordinator_max_concurrent`). Agent definitions load from builtins + `<workspace>/.orangutan/agents/`.
- **swarm/** — `AgentMailbox` (SQLite) for inter-agent messages, `TeamManager` (SQLite) for team membership. Workers poll the mailbox via an injected `incoming_message_fetcher` each loop iteration.
- **automation/** — cron/periodic/triggered jobs persisted in SQLite. `AutomationRuntime` has an executor callback that builds a fresh agent runtime for each firing; categories (e.g. `heartbeat`) register custom runners. Notifier callback routes output to cli/channel/web.

### Channels, web, CLI

- **channel/** — `ChannelManager` dispatches inbound messages into `MessageQueue`, `JidTaskRunner` fans out to per-JID task pools. `channel/qq/` is the QQ platform adapter (gated by `qq_channel` option at compile time).
- **web/** — cpp-httplib server. Routes split across `chat-routes`, `session-routes`, `admin-routes*`, `event-stream` (SSE), shared `WebContext`/`EventBus`. `web-route-internal.hpp` collects internals.
- **cli/** — replxx-backed REPL (`repl.cpp`), single-shot mode (`single-shot.cpp`), slash commands (`slash-commands.cpp`), session restore/save workflow.

### State layer

- **storage/** — `SessionStore` (SQLite) persists conversation history per session. `sqlite.hpp` / `sqlite-throwing.hpp` / `sqlite-error.hpp` — the canonical API is **`std::expected`**-based (`SqliteResult<T> = std::expected<T, SqliteError>`); throwing wrappers exist for tight callsites but the migration goal is explicit expected. New SQLite code must use the expected API.
- **memory/** — `MemoryStore` (SQLite) + `RuntimeMemory` scope wrapper + `memory-search` + `memory-mirror` (optional `.orangutan/memory/MEMORY.md` mirror for human inspection) + `memory-age` (decay/retention).
- **config/** — JSON (`config.example.json` is the shape). Secrets (`api_key`, `client_secret`, etc.) are encrypted at rest under a password (`secret-protection-*`, `secret-fields`). `Config::load` accepts `ConfigSecretOptions` for password override. Do not log or echo decrypted secrets.
- **bootstrap/identity.cpp** — derives per-runtime identity keys (`runtime_key`, `memory_scope`) used to namespace memory and notifications.

### Request flow (CLI mode, one message)

```
user input → cli/repl or cli/single-shot
  → AgentLoop::run(prompt)
     → hook_manager (pre-iteration)
     → ProviderSystem::send (transport → protocol adapter → runtime backend)
     → response parsed into Message + ToolUse list
     → for each ToolUse: permissions check → tool dispatch → ToolResult
     → append to history; loop until stop_reason = end_turn or MAX_ITERATIONS
  → session_store persists history (if auto_save) → memory distillation (optional)
```

Channel mode replaces the input source with `MessageQueue`; web mode exposes the same loop via HTTP/SSE.

### Where things are assembled

`bootstrap/runtime-assembler.cpp` builds an `AgentRuntimeBundle` (agent + provider + tools + hook_manager) from a `RuntimeAssemblyRequest`. Three assembly sites: primary CLI runtime, coordinator worker runtimes, automation-triggered runtimes. When adding a runtime-wide capability, thread it through `RuntimeAssemblyRequest` — don't reach into globals.

## Code Constraints & Conventions

Rules, idioms, and style guidance live in [`.claude/rules/`](.claude/rules/README.md). Read the file that matches the task:

- [`.claude/rules/critical-rules.md`](.claude/rules/critical-rules.md) — non-negotiable constraints (no macros, no implicit bool, explicit single-arg ctors, treat clang-tidy warnings as errors, no custom thread pools, new SQLite code uses `std::expected`, do not log decrypted secrets, etc.). **Read before any C++ edit.**
- [`.claude/rules/design-principles.md`](.claude/rules/design-principles.md) — integration, composability, sender/receiver, error handling, wrapper APIs, testing expectations, `src/utils/` reuse.
- [`.claude/rules/code-style.md`](.claude/rules/code-style.md) — modern C++ idioms, type/memory choices, naming conventions, include order, performance rules, spdlog conventions.
- [`.claude/rules/workflow.md`](.claude/rules/workflow.md) — branch/commit workflow, pre-commit hook, preferred CLI tools (`rg`, `fd`, etc.).
- [`.claude/rules/libraries.md`](.claude/rules/libraries.md) — reference for integrated third-party libraries.

When rules or architecture shift, update the corresponding `.claude/rules/*.md` file and this document.