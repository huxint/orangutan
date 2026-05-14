# harness-engineering

**Harness-engineering** is the agent-first scaffold for **Orangutan v2**, the C++23 rewrite
of the [`orangutan/`](../) project — a single-binary LLM ReAct agent runtime with pluggable
providers, a tool registry, multi-platform chat channels, persistent memory, an HTTP web UI,
an orchestration runtime that spawns and coordinates worker agents, and a cron-like
automation engine.

This repository's primary deliverable is **the prompt framework**: a versioned set of design
documents, rules, templates, and mechanical checks that allow a coding agent (Claude Code,
Codex, etc.) to build the C++ project from zero **without depending on chat memory**.

The framework draws on [`iFurySt/harness-template`](https://github.com/iFurySt/harness-template)
(short `AGENTS.md` routing layer, `docs/` system of record, exec-plans + histories +
product-specs convention) and extends it with C++23-specific compile-time and
agent-runtime concerns.

## Why a rewrite?

The legacy `orangutan/` codebase reached ~40 kLoC C++23 and the following walls:

- **Compile times** of ~70s clean / minutes of incremental on 16 GB RAM, driven by an
  NVIDIA `stdexec` fork that exists in five files but is transitively visible in the
  agent core; by heavy header-only includes (`nlohmann_json`, `cpp-httplib`, `spdlog/fmt`,
  `ctre`) reaching deep into translation units; and by absence of PCH / modules / unity
  builds.
- **Channel monolith** — ~3.6 kLoC of QQ-specific code with no extracted `Channel` trait,
  blocking Discord / Slack / Telegram / Webhook adapters.
- **File-vs-shell tool duplication** — ~2.2 kLoC of overlapping glob/ls/mkdir/mv code paths.
- **Mixed SQLite error model** — `std::expected`-based API is the stated goal but ~120
  call sites still use throwing wrappers.
- **Hooks** cover only tool-lifecycle and message events; memory, automation, orchestration,
  and provider events have no hookable surface.
- **Single-threaded web server** (`cpp-httplib`) caps the HTTP UI at ~10–50 concurrent
  clients.
- **No benchmark discipline** — there is no place where alternative implementations are
  compared on real workloads.

`docs/references/orangutan-legacy-audit.md` contains the full audit.

## What's different in v2

- **Compile budget enforced.** Each TU has a target; CI tracks per-file compile time;
  PCH + C++23 modules + strict include hygiene are mandatory not optional.
  See `docs/rules/compile-budget.md`.
- **GCC 16.1 primary toolchain.** Modules (`import oran.core;`), `std::expected`,
  `std::generator`, deducing-`this`. Clang ≥ 19 is the fallback.
- **Async = asio standalone + C++20 coroutines.** No `stdexec` fork. One executor type
  threaded everywhere. See `docs/design-docs/async-model.md`.
- **Channel trait.** `orangutan::channel::Channel` is the only thing the agent runtime
  knows about. QQ, Discord, Slack, Telegram, Webhook, WebSocket, and custom adapters all
  implement it. See `docs/design-docs/channel-abstraction.md`.
- **Provider trait with a capability matrix.** Anthropic Messages, OpenAI Chat
  Completions, OpenAI Responses, Gemini, DeepSeek, and OpenAI-compatible custom
  endpoints share one adapter shape. See `docs/design-docs/api-portability.md`.
- **Tiered memory.** Working / session / long-term / team-shared, each with its own
  backend trait, retention policy, and hookable lifecycle.
  See `docs/design-docs/memory-system.md`.
- **Hook checkpoints everywhere.** Tool, agent, provider, memory, channel, and
  orchestration lifecycles each expose named pre/post points. Multiple sink kinds —
  shell, embedded Lua, in-process C++ — all conform to the same `HookSink` interface.
  See `docs/design-docs/permissions-and-hooks.md`.
- **Agent team collaboration as a first-class primitive.** Teams have explicit
  coordination strategies (leader-worker, pipeline, voting, free-form mailbox),
  shared scratchpad memory, and a conversation DAG. See
  `docs/design-docs/team-collaboration.md`.
- **`bench/` neighbours `tests/`.** Each library has both. Every meaningful design choice
  with a plausible alternative ships a comparison bench. See
  `docs/product-specs/0010-benchmark-harness.md`.

## Repository Layout

```
harness-engineering/
├── AGENTS.md                       routing layer (read first)
├── CLAUDE.md                       one-liner: read AGENTS.md
├── README.md                       this file
├── CONTRIBUTING.md                 collaboration model
├── SECURITY.md                     vulnerability reporting
├── Makefile                        init / check / new-plan / new-history / bench
├── docs/
│   ├── REPO_COLLAB_GUIDE.md        working agreement
│   ├── ARCHITECTURE.md             target architecture map
│   ├── BUILD_SYSTEM.md             xmake + GCC 16.1 + modules + PCH
│   ├── FAST_COMPILATION.md         compile-time engineering playbook
│   ├── CICD.md                     CI/CD scaffolding
│   ├── SECURITY.md                 secure defaults
│   ├── SUPPLY_CHAIN_SECURITY.md    deps / SBOM / provenance
│   ├── RELIABILITY.md              ops, logs, retries
│   ├── PRODUCT_SENSE.md            product principles
│   ├── QUALITY_SCORE.md            quality matrix
│   ├── HISTORY_GUIDE.md            change-history convention
│   ├── PLANS_GUIDE.md              execution-plan convention
│   ├── FRONTEND.md                 UI surface conventions
│   ├── design-docs/                deep architectural designs
│   ├── rules/                      non-negotiable rules
│   ├── product-specs/              user-visible feature specs
│   ├── exec-plans/                 active / completed plans + templates
│   ├── histories/                  finished-task records
│   ├── references/                 curated external + legacy material
│   ├── releases/                   user-visible release notes
│   └── generated/                  reproducible generated artifacts
├── scripts/                        repo automation (ci, new-plan, new-history, bench-compare)
├── skeleton/                       starting xmake/include/src skeleton for the new project
├── include/                        (target) public C++ headers
├── src/                            (target) C++ implementation
├── tests/                          Catch2 buckets — one per library
├── bench/                          nanobench/Catch2 buckets — one per library
└── .github/                        workflows, PR template, issue templates
```

## Quick Start

1. **Read [`AGENTS.md`](AGENTS.md).**
2. Open [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the v2 target.
3. Open [`docs/references/orangutan-legacy-audit.md`](docs/references/orangutan-legacy-audit.md)
   to absorb why the rewrite exists.
4. For implementation work, create an execution plan:

   ```sh
   make new-plan SLUG=core-react-loop
   ```

5. When the change is finished, record a history entry:

   ```sh
   make new-history SLUG=core-react-loop-mvp
   ```

6. Before opening a PR:

   ```sh
   make ci
   ```

## License

Proprietary — same posture as legacy `orangutan/`. Update before publishing.
