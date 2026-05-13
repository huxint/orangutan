# 🦧 Orangutan

**Orangutan** is a C++23 agent assistant — a single binary that runs an LLM [ReAct](https://arxiv.org/abs/2210.03629) loop with a pluggable tool registry, multiple LLM provider backends, persistent memory and sessions, an HTTP web UI, external chat channels (QQ), an orchestration runtime that spawns and coordinates worker agents, and a cron-like automation engine.

## Features

- **ReAct Agent Loop** — Iterative LLM reasoning with tool call dispatch, permission enforcement, and hook integration.
- **Multi-Provider Support** — Anthropic Messages API, OpenAI Chat Completions, OpenAI Responses, with protocol adapters and fallback/retry logic.
- **Rich Tool Ecosystem** — File I/O, shell execution, MCP clients, memory management, orchestration, automation, skills, web search, and more.
- **Orchestration** — Spawn and manage multiple specialized agents (coder, researcher, etc.) with mailbox-based messaging and team collaboration.
- **Automation** — Cron/periodic/triggered jobs persisted in SQLite with per-category custom runners.
- **Persistent State** — SQLite-backed session history and long-term memory with decay/retention, plus optional MEMORY.md mirror.
- **Multi-Interface** — CLI (REPL + single-shot), HTTP web UI with SSE streaming, and QQ bot channels.
- **Permissions System** — Fine-grained allow/deny/ask rules with signed approval prompts.
- **Secret Protection** — API keys and secrets encrypted at rest with password-based decryption.

## Quick Start

### Prerequisites

- **C++23** compiler (GCC 14+ or Clang 18+)
- **[xmake](https://xmake.io/)** build system
- **SQLite3**, **libcurl**, and other dependencies (auto-fetched by xmake)

### Build

```bash
# Configure
xmake f -m release

# Build the binary
xmake build orangutan

# Or build everything (slow)
xmake
```

### Configuration

Copy and edit the example config:

```bash
cp config.example.json config.json
```

Fill in your API keys and adjust settings. Secrets support `${ENV_VAR}` substitution and encrypted at-rest storage.

### Run

```bash
# Interactive REPL
./orangutan

# Single-shot query
./orangutan --prompt "Explain the builder pattern in C++"

# With custom config
./orangutan --config /path/to/config.json

# Web UI
./orangutan --web --port 8080

# Disable QQ channel (if compiled in)
xmake f --qq_channel=n
```

### Tests

```bash
# Run all tests
xmake test

# Run a specific test bucket
xmake run test-agent "[tag]"

# Single test case
xmake run test-agent "test case name"
```

## Architecture Overview

```
src/
├── agent/          # ReAct agent loop
├── automation/     # Cron-like job scheduling
├── bootstrap/      # Runtime assembler (main entry point)
├── channel/        # QQ bot integration
├── cli/            # REPL, single-shot, slash commands
├── config/         # JSON config + secret encryption
├── memory/         # Long-term memory (SQLite)
├── orchestration/  # Multi-agent coordination
├── permissions/    # Tool access control
├── providers/      # LLM backends (Anthropic, OpenAI)
├── skills/         # Skill loading system
├── storage/        # Session persistence (SQLite)
├── tools/          # Tool registry and implementations
├── types/          # Shared type definitions
├── utils/          # Utilities (expected, task pool, etc.)
└── web/            # HTTP server + web UI
```

### Request Flow

```
user input → CLI / Web / QQ Channel
  → AgentLoop::run(prompt)
    → HookManager (pre-iteration)
    → ProviderSystem::send (transport → protocol → backend)
    → Parse response → Permission check → Tool dispatch
    → Loop until end_turn or MAX_ITERATIONS
  → Session save → Memory distillation
```

## Configuration

See `config.example.json` for the full configuration shape. Key sections:

| Section | Description |
|---------|-------------|
| `agent` | Default agent profile, model, workspace |
| `permissions` | Tool access rules: allow/deny/ask lists |
| `profiles` | LLM provider configurations (base URL, API key, models) |
| `agents` | Per-agent overrides (permissions, thinking budget, concurrency) |
| `session` | Auto-save and session settings |
| `security` | Client allow/deny lists |
| `qq_bots` | QQ bot definitions |

## Development

- **Build system**: xmake (not CMake)
- **Test framework**: Catch2
- **Formatting**: clang-format (pre-commit hook)
- **Linting**: clang-tidy (treat warnings as errors)
- **LSP**: clangd (compile_commands.json auto-generated)

See [AGENTS.md](AGENTS.md) for detailed architecture and conventions.

## License

Proprietary. All rights reserved.
