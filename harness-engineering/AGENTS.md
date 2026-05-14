# AGENTS.md

This repository is the **harness-engineering** scaffold for **Orangutan v2** — a C++23
agent-runtime rewrite of the original `orangutan/` project. It is *agent-first*: every
load-bearing decision is recorded in a versioned file under `docs/` so that Claude Code,
Codex, or any future agent can ship features without depending on chat memory.

`AGENTS.md` is a **routing layer**, not the encyclopedia. Read the files it points at.

> **The Prime Directive — docs match reality.**
>
> Every change to behavior, build, configuration, dependencies, interfaces, file layout,
> commands, or conventions **must update the corresponding documentation in the same
> change**. A PR that ships code without updating the docs it invalidates is *incomplete*
> and will be rejected. There is no "I'll update the doc later" — the doc *is* the
> change. See [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md) for the
> mechanics and [`docs/rules/critical-rules.md#C16`](docs/rules/critical-rules.md) for
> the rule.
>
> If a rule under `docs/rules/` cannot be honored, propose an edit to the rule first —
> do not silently break it.

---

## Read At The Start Of Every Task

| File | Why |
| --- | --- |
| `docs/REPO_COLLAB_GUIDE.md` | Repository-wide working agreement, commit/PR/test expectations. |
| `docs/ARCHITECTURE.md` | Target architecture map, library boundaries, binary inventory. |
| `docs/design-docs/core-beliefs.md` | Non-negotiable operating principles. |
| `docs/rules/critical-rules.md` | Non-negotiable C++/build constraints. **Read before any code edit.** |
| `docs/rules/compile-budget.md` | Compile-time budget per TU and per target. **The previous project failed here.** |
| `docs/rules/docs-in-sync.md` | The Prime Directive: docs must match reality. What to update, when, how. |

## Read Before Finishing A Code Change

| File | Why |
| --- | --- |
| `docs/HISTORY_GUIDE.md` | When and how to record finished tasks under `docs/histories/`. |
| `docs/QUALITY_SCORE.md` | Current quality targets and gaps by area. |
| `docs/rules/testing-and-bench.md` | What counts as a passing change (tests **and** bench impact). |

## Read When The Task Touches A Module

| Area | File |
| --- | --- |
| Build / xmake / GCC 16.1 | `docs/BUILD_SYSTEM.md`, `docs/rules/module-and-pch.md` |
| Compile-time pressure | `docs/FAST_COMPILATION.md`, `docs/rules/compile-budget.md` |
| Async, executors, coroutines | `docs/design-docs/async-model.md`, `docs/rules/async-and-concurrency.md` |
| Error handling | `docs/rules/error-handling.md` |
| Tools / hooks / permissions | `docs/design-docs/tool-runtime.md`, `docs/design-docs/permissions-and-hooks.md` |
| Channels (QQ / Discord / Slack / Webhook / …) | `docs/design-docs/channel-abstraction.md` |
| LLM provider portability | `docs/design-docs/api-portability.md` |
| Memory (working / session / long-term / shared) | `docs/design-docs/memory-system.md` |
| Agent team collaboration | `docs/design-docs/team-collaboration.md` |
| Secrets / config / state | `docs/design-docs/secrets-and-state.md` |
| Module / TU boundaries | `docs/design-docs/module-boundaries.md` |
| Skills | `docs/product-specs/0009-skills.md` |
| Automation (cron / periodic / triggered) | `docs/product-specs/0006-automation.md` |
| Web UI surface | `docs/product-specs/0007-web-ui.md`, `docs/FRONTEND.md` |
| Permissions | `docs/product-specs/0008-permissions.md` |
| Benchmark harness | `docs/product-specs/0010-benchmark-harness.md` |
| Reliability / observability | `docs/RELIABILITY.md` |
| Security / supply chain | `docs/SECURITY.md`, `docs/SUPPLY_CHAIN_SECURITY.md` |
| CI/CD | `docs/CICD.md` |
| External references | `docs/references/` |
| Legacy `orangutan/` lessons | `docs/references/orangutan-legacy-audit.md` |

## Working Rules

- **Docs match reality, always.** Every behavior, build, config, dep, interface, file
  layout, command, or convention change updates the matching docs in the same PR. See
  [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md). This is non-negotiable.
- Prefer **small, explicit, repository-legible abstractions** over clever metaprogramming.
- Keep prompts, policies, and architectural rules versioned in-repo.
- For non-trivial work (multiple commits, migration risk, architectural impact) **create an
  execution plan** under `docs/exec-plans/active/` before writing code.
- Record finished code-change tasks in `docs/histories/YYYY-MM/`.
- **Compile time is a feature.** A clean build must stay under the budget in
  `docs/rules/compile-budget.md`. Every PR is responsible for not pushing it past the budget.
- **Hooks are pluggable, not magical.** Every lifecycle point that *could* dispatch a hook
  is enumerated in `docs/design-docs/permissions-and-hooks.md`.
- **No code is special.** Every library has both `tests/` and `bench/` neighbours.

## Conventions At A Glance

- **Language:** C++23, ratcheting toward C++26 features that GCC 16.1 ships stable.
- **Compiler:** GCC 16.1 primary, Clang ≥ 19 secondary (CI runs both when feasible).
- **Build:** xmake. Lock file pinned. PCH on, modules where supported, unity for cold modules.
- **Async:** standalone asio + C++20 coroutines. **No NVIDIA stdexec.** **No std::thread, no custom thread pool.**
- **Error model:** `std::expected<T, Error>` end-to-end. Throwing wrappers exist only for `main`-level shims and are explicitly named.
- **Logging:** spdlog, configured with `SPDLOG_FMT_EXTERNAL=1` against header-only `fmt`. Use `oran::log::*` shim, not raw spdlog macros, so the logger is mockable in tests.
- **JSON:** `nlohmann::json_fwd.hpp` in headers; full include in `.cpp` only.
- **Strings:** UTF-8 by contract; conversion handled at boundaries via `oran::core::str::*`.
- **Tests:** Catch2 v3, one bucket per library, `tests/<lib>/...`.
- **Benches:** nanobench + Catch2 runners, one bucket per library, `bench/<lib>/...`. Each bench bucket must own at least one **A-vs-B comparison** documenting the tradeoff.
- **Histories** are required for every code-change task that modifies behavior.

## Quick Commands

```sh
make ci                  # docs + hygiene checks (pre-PR gate)
make new-plan SLUG=...   # scaffold an execution plan
make new-history SLUG=...# scaffold a history entry
make check-docs          # verify required docs exist
```

Once the C++ build is set up (per `docs/BUILD_SYSTEM.md`):

```sh
xmake f -m release && xmake build orangutan         # build the binary
xmake test                                          # run every test-* target
xmake build bench-<lib> && xmake run bench-<lib>    # run a benchmark
scripts/bench-compare.sh                            # cross-implementation benchmark report
```

## What This Repository Is Not

- It is **not** a fork of `orangutan/`. The legacy tree is reference material, audited in
  `docs/references/orangutan-legacy-audit.md`. Do not copy code without justification.
- It is **not** a place for ad-hoc one-shot scripts. Anything reusable belongs under
  `scripts/` with the same lint rules as production code.
- It is **not** a place for free-form notes. If a piece of knowledge will outlive the chat,
  put it in `docs/`.
