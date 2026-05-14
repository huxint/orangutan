# Workflow

Git, branch, and CLI tool conventions. Process-level rules; not C++ rules.

## Branching

- Default branch: `main`.
- Feature branches: `feat/<short-slug>`, `fix/<short-slug>`, `refactor/<short-slug>`,
  `docs/<short-slug>`, `perf/<short-slug>`, `build/<short-slug>`.
- Long-lived branches: rare; if a feature spans weeks, do it behind a feature flag on
  `main` rather than a long-lived branch.

## Commits

- One logical change per commit.
- Subject line ≤ 70 chars, imperative ("add hook bus", not "added hook bus").
- Body explains *why*, optional but encouraged for non-trivial changes.
- Trailer `Co-Authored-By: ...` when relevant.
- Sign with GPG if your environment is set up for it; otherwise unsigned is fine.

### Subject prefixes

We use Conventional Commits prefixes:

| Prefix     | Use for                                        |
| ---------- | ---------------------------------------------- |
| `feat`     | New user-visible feature                        |
| `fix`      | Bug fix                                         |
| `refactor` | Internal restructuring with no behavior change  |
| `perf`     | Performance improvement                         |
| `docs`     | Docs-only change                                |
| `test`     | Tests-only change                               |
| `bench`    | Benchmark-only change                           |
| `build`    | Build system / packaging / xmake change         |
| `ci`       | CI configuration change                          |
| `chore`    | Routine maintenance (deps bump, cleanup)        |

Example: `feat(agent): add cancellation handling to Loop::run`.

## Pull Requests

- Open against `main`.
- PR title repeats the commit-prefix style if the PR has one commit; otherwise a
  brief subject.
- PR description follows `.github/PULL_REQUEST_TEMPLATE.md`.
- **Required for PRs**:
  - `make ci` passes locally.
  - `xmake build` passes locally for affected targets.
  - `xmake test` passes locally for affected buckets.
  - `scripts/check-compile-budget.sh` is green for affected libraries.
  - History entry added (or `History-Skip: <reason>` trailer in PR description).
- **Encouraged for PRs**:
  - Bench run results (when perf-relevant).
  - Screenshots / asciinema (when UI/CLI-relevant).
  - Cross-link to exec plan or design doc.

## Reviews

- Reviewers cite repository paths, not chat context.
- Defer style nitpicks to the formatter; review for correctness, architecture, and
  rule compliance.
- Use suggestion blocks for small fixes; for non-trivial changes, ask for a revised
  PR rather than amending in review comments.

## CLI Tools

The repo expects these on `$PATH`:

- **xmake** ≥ 2.9
- **gcc-16** (or **g++-16**) — primary compiler
- **clang-19+** — secondary compiler (CI matrix)
- **rg** (ripgrep) — preferred over `grep`
- **fd** — preferred over `find`
- **jq** — for JSON-aware scripts
- **bat** — optional, for readable file display
- **clang-format**, **clang-tidy** — match GCC 16.1's clang version (19.x baseline)
- **markdownlint-cli2** — for doc lint
- **shellcheck** — for shell-script lint
- **shfmt** — for shell-script format

When agents shell out, they use `rg` and `fd` by default; `grep` and `find` are
acceptable when ripgrep is unavailable.

## Pre-Commit Hook

`.githooks/pre-commit` (shipped in repo):

- Runs `clang-format` on staged C++ files.
- Runs `shfmt -d` on staged `.sh` files.
- Runs `scripts/check-secret-logs.sh` on the diff.

Install:

```sh
git config core.hooksPath .githooks
```

CI also runs all checks regardless of local hook state.

## Issue Triage

- Issues use the templates under `.github/ISSUE_TEMPLATE/`.
- `bug_report` — produce a minimal reproducer.
- `feature_request` — link to the relevant design doc or product spec.
- Labels: `bug`, `feature`, `perf`, `compile-time`, `docs`, `build`, `agent-runtime`,
  `channel`, `provider`, `memory`, `automation`, `hooks`, `permissions`, `web`, `cli`.

## Release Cadence

- v1 milestone:  MVP runtime.
- v1.x:           channel adapters, hook sinks, agent team v1.
- v2:             modules everywhere; vector memory; stretch goals.

Release process is documented in `docs/CICD.md` once the binary is real. Until then,
`docs/releases/feature-release-notes.md` is the canonical record.

## When Things Are Broken

- `main` broken: revert first, fix forward. Don't paper over.
- A CI check is wrong: open a PR fixing the check rather than disabling it.
- A test is "flaky": investigate before disabling. Flakiness is a real bug; we just
  don't always have the time to fix it immediately. Track in
  `docs/exec-plans/tech-debt-tracker.md`.

## See Also

- [`testing-and-bench.md`](testing-and-bench.md)
- [`../HISTORY_GUIDE.md`](../HISTORY_GUIDE.md)
- [`../PLANS_GUIDE.md`](../PLANS_GUIDE.md)
