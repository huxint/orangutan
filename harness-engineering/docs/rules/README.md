# Rules

`docs/rules/` is the **non-negotiable layer**. Every file here describes a rule that
agents and humans must follow. Every rule has:

- A short statement.
- A rationale.
- A list of mechanical checks that enforce it (or "no automated check yet — review
  manually").
- An example of compliance and one of non-compliance.

If a rule blocks your work, **edit the rule and document the reasoning in the PR**.
Do not silently break it.

## Read These Before Touching Code

| File | Topic |
| --- | --- |
| [`critical-rules.md`](critical-rules.md) | The hard "thou shalt not" set. |
| [`docs-in-sync.md`](docs-in-sync.md) | **The Prime Directive — docs must match reality, every PR.** |
| [`code-style.md`](code-style.md) | Naming, formatting, idioms. |
| [`compile-budget.md`](compile-budget.md) | Per-TU + per-target compile-time limits. |
| [`module-and-pch.md`](module-and-pch.md) | Public header, PCH, module rules. |
| [`error-handling.md`](error-handling.md) | `std::expected`; no cross-boundary throws. |
| [`async-and-concurrency.md`](async-and-concurrency.md) | asio + coroutines; no `std::thread`. |
| [`libraries.md`](libraries.md) | Approved 3rd-party libraries. |
| [`testing-and-bench.md`](testing-and-bench.md) | Tests + benches expected per change. |
| [`workflow.md`](workflow.md) | Git, CLI tools, branch naming. |

## When To Update Rules

- A rule no longer matches reality → update the rule and link the PR.
- A rule blocks a legitimate need → update the rule, propose the relaxed version.
- A rule has *no enforcement script* → consider adding one in `scripts/check-*.sh`.

## Anti-Patterns

- A "guideline" file that has no enforcement and is occasionally cited. Either it's a
  rule (with enforcement) or it's not. Keep `docs/rules/` honest.
- Rules that depend on a person to enforce. Mechanical checks > human review for
  things humans get bored of.
