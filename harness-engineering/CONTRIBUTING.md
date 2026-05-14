# Contributing

This repository is designed for **agent-first** development; the same rules apply to
humans and bots. The shorter version lives in `AGENTS.md`; this file is the long
form.

## Working Agreement

- Start from `AGENTS.md`, then read the linked docs that match the task.
- Keep repository knowledge in versioned files, not only in chat or ticket comments.
- **Docs match reality, always.** If behavior, build, config, dependencies, interfaces,
  file layout, commands, or conventions change, **update the matching docs in the same
  PR**. See [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md) and
  [`docs/rules/critical-rules.md#C16`](docs/rules/critical-rules.md). There is no
  "doc update follow-up PR".
- For large or risky work, create an execution plan under `docs/exec-plans/active/`
  before writing code.
- Follow `docs/rules/*.md`; if a rule blocks legitimate work, edit the rule in the
  same PR.

## Before Opening A Pull Request

- Run `make ci`.
- `xmake build` passes for the affected targets.
- `xmake test` passes for the affected buckets.
- `scripts/check-compile-budget.sh` green for the affected libraries (once the
  build skeleton lands).
- **Every doc that the change invalidates is updated** — see
  [`docs/rules/docs-in-sync.md`](docs/rules/docs-in-sync.md) for the
  change-type → docs-to-update map.
- Add or update a history entry under `docs/histories/YYYY-MM/` if the task
  changed repository code or workflow. Use `make new-history SLUG=...`.
- Update `docs/releases/feature-release-notes.md` if the change is user-visible.
- Verify examples and scripts still match the current behavior.
- Re-read the rule files that apply to the area you touched.

## Review Expectations

- Prefer small, scoped pull requests (≤ ~600 lines / ~6 files).
- Call out risks, migrations, and deferred follow-ups explicitly.
- Link to the relevant plan, design doc, spec, or history file when context is
  important.
- Reviewers cite repository paths, not chat context.
- Style nits are deferred to formatters; review for correctness, architecture,
  and rule compliance.

## Setup

```sh
# Toolchain
sudo apt install gcc-16 g++-16   # or your distro's equivalent
curl -fsSL https://xmake.io/shget.text | bash

# Pre-commit hook
git config core.hooksPath .githooks

# First configure
xmake f -m release

# Sanity check
make ci
```

## Communication

- GitHub Issues: bug reports, feature requests, security notices.
- Pull Requests: code, doc, and scaffold changes.
- Discussions: design questions before opening an exec plan (optional).

## See Also

- [`AGENTS.md`](AGENTS.md)
- [`docs/REPO_COLLAB_GUIDE.md`](docs/REPO_COLLAB_GUIDE.md)
- [`docs/rules/workflow.md`](docs/rules/workflow.md)
