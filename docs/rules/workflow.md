# Git Workflow

Use a focused branch from `main`. Commit each complete, verified slice with an
imperative Conventional Commit subject of at most 70 characters. The body
explains the reason and any material limit; Git is the change history.

Before committing, run affected build/tests and `make ci`, update changed
contracts, inspect the diff and remove obsolete callers/fixtures. Large changes
must have an [execution plan](../PLANS_GUIDE.md).

The optional `.githooks/pre-commit` formats staged C++ and checks shell/secret
hygiene. It refuses partly staged C++ files rather than widening their scope.
Install with `git config core.hooksPath .githooks` when using it.

PRs describe the resulting behavior, validation and remaining risks for a reader
without the conversation. Use the repository template. Review feedback cites
current contracts or source files. Never add a duplicate history ledger.
