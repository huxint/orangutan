# Execution Plans Guide

Use **execution plans** for tasks that are too large, risky, or stateful to manage
through short chat context.

## When To Create A Plan

Create one when **any** of these apply:

- The task spans multiple commits or working sessions.
- The work touches more than ~6 files or ~600 lines.
- There is real architectural impact (new library, new public API, dependency change).
- Multiple contributors or agents may touch the area over time.
- Migrating from a legacy shape (e.g., porting QQ adapter to v2 trait).
- Success depends on staged rollout or measurement checkpoints (compile-time
  improvements, perf changes).

## Storage

- Active plans: `docs/exec-plans/active/`.
- Completed plans: `docs/exec-plans/completed/`.
- Template: `docs/exec-plans/templates/execution-plan.md`.
- Ongoing tech debt: `docs/exec-plans/tech-debt-tracker.md`.

Create a new plan with:

```sh
make new-plan SLUG=mvp-react-loop
```

This drops `docs/exec-plans/active/<YYYY-MM-DD>-mvp-react-loop.md` from the template.

## Expected Sections

The template encodes:

- **Goal** — desired end state.
- **Scope** — in/out of scope.
- **Context** — relevant docs and code paths.
- **Risks** + mitigations.
- **Milestones**.
- **Validation** — commands, manual checks, observability checks.
- **Progress Log** — checklist with timestamps.
- **Decision Log** — dated decisions + rationale.

## Maintenance

- Update the plan as decisions change. Reviewers and the next agent will read the
  *current* state, not the original.
- Move to `completed/` when shipping. Do not delete; the history is valuable.
- If the plan goes stale (no progress in 30 days, no longer relevant) archive it to
  `completed/` with a closing note.

## Anti-Patterns

- Plans that describe *implementation* in detail before any implementation. State
  *intent*, not source code. Source code goes in the PR.
- Plans that bypass design docs. If you find yourself defining a public API in a
  plan, the design doc should host that decision; the plan references it.
- "Living" plans that never close. Close them; open a follow-up if needed.

## See Also

- [`exec-plans/README.md`](exec-plans/README.md)
- [`HISTORY_GUIDE.md`](HISTORY_GUIDE.md)
