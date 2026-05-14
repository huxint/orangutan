# Repository Collaboration Guide

This guide defines the default working model for everyone — humans and agents — on this
repository. Stack-specific rules live in `docs/rules/`; this file is the meta layer.

## Development Principles

- **Repository legibility beats clever prompting.** If a fact only exists in chat,
  tickets, or human memory, it effectively does not exist. Write it down.
- **Fix the environment, not the prompt.** When an agent fails repeatedly at the same
  task, treat it as a missing doc, a missing script, or a missing test — not as a
  prompt-engineering deficit.
- **Throughput vs. entropy.** Velocity is welcome; uncleaned debt is not. Land
  simplification along with each feature, or open a `tech-debt-tracker.md` entry.
- **Mechanical checks over soft conventions.** Wherever a rule exists, add a script that
  enforces it. `scripts/check-*.sh` is the right home.
- **Small, scoped changes.** Prefer multiple small PRs over one mega-PR. A PR that
  touches more than ~600 lines or more than ~6 files almost always wants an execution
  plan first.
- **Treat docs as code.** Markdown is reviewed, tested (markdownlint, `check-docs.sh`),
  and updated in the same commit as the behavior it describes.

## Documentation Discipline

**Docs are part of every change. This is the Prime Directive — see
[`rules/docs-in-sync.md`](rules/docs-in-sync.md) and
[`rules/critical-rules.md#C16`](rules/critical-rules.md).**

- A PR that changes behavior, build, config, dependencies, interfaces, file layout,
  commands, or conventions **must update the matching docs in the same PR**. Doc
  updates are not a follow-up.
- `AGENTS.md` is a routing layer — keep it short.
- `docs/` is the source of truth. Prefer adding a new focused doc to bloating a catch-all.
- Cross-link liberally; agents navigate via links.
- Code samples in docs are short, copy-pastable, and labeled with a target file path.
- When behavior changes, update the corresponding doc(s) in the same change.
- Stale docs are bugs. If you read a doc that no longer matches reality, fix it or open a
  history entry noting the gap.
- Reviewers reject PRs that ship code drift. A "looks good, please open a doc PR after"
  is **not** an accepted review outcome.

## Git And Review

- One logical change per commit; one logical change per PR when possible.
- Commit subject ≤ 70 chars; body explains *why*, not *what*.
- Branch from `main`; rebase before merge; squash if commits are noisy.
- Before a commit or PR, verify that docs, examples, scripts, and histories reflect the
  final behavior. `make ci` catches the basics.
- Large or risky work lands behind an execution plan checked into `docs/exec-plans/`.
- Review comments cite repository files, not private context: "see
  `docs/design-docs/async-model.md#scheduler-ownership`".

## Coding Standards (Pointers)

- `docs/rules/critical-rules.md` — non-negotiables.
- `docs/rules/code-style.md` — idioms, naming, formatting.
- `docs/rules/compile-budget.md` — per-TU and per-target compile-time budget.
- `docs/rules/module-and-pch.md` — modules / PCH / include hygiene.
- `docs/rules/async-and-concurrency.md` — asio + coroutines, no `std::thread`.
- `docs/rules/error-handling.md` — `std::expected` end-to-end.
- `docs/rules/libraries.md` — approved third-party libraries and their boundaries.
- `docs/rules/testing-and-bench.md` — what counts as a passing change.
- `docs/rules/workflow.md` — git, tooling, branch rules.

## Testing And Validation

- Every meaningful code change leaves the project with **stronger verification than
  before** — a new test, a new bench, a new check, or a tightened existing one.
- Tests live under `tests/<library>/...`; benches under `bench/<library>/...`.
- The project has two kinds of "tests" for performance:
  1. **Correctness tests** (Catch2) — `xmake run test-<lib>`
  2. **Benchmarks** (nanobench / Catch2 benchmark macros) — `xmake run bench-<lib>`
- Benchmarks must compare at least one alternative when a design tradeoff exists. See
  `docs/product-specs/0010-benchmark-harness.md`.
- CI runs `xmake test` on every PR. Bench jobs run on nightly + on label
  `perf-impact`.

## CI/CD And Release Posture

- `scripts/ci.sh` is the only mandatory base gate; it runs doc + hygiene checks.
- Per-library C++ tests run via xmake.
- `make release-package` (placeholder) eventually wraps the real build.
- See `docs/CICD.md` for the full pipeline plan.

## Configuration Hygiene

- Examples (`config.example.json`) and runtime defaults stay aligned. CI verifies the
  example loads without secrets.
- Every required environment variable is documented in `docs/RELIABILITY.md` under
  "Required environment".
- Secrets are never logged. The logging shim in `oran::log::*` redacts known secret
  fields based on `docs/design-docs/secrets-and-state.md`.
- No hidden setup steps. Anything an agent must do to bootstrap belongs in `scripts/` or
  versioned markdown.

## When You Disagree With A Rule

- Open a PR that edits the rule with a rationale paragraph.
- Land it before — or alongside — the code that depends on the new shape.
- Reference the PR in the affected history entry.
