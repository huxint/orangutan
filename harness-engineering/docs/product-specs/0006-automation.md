# 0006 — Automation Engine

## User Problem

Operators want recurring agent work: "every morning at 9 am, summarize overnight CI
runs", "every 30 minutes, scan for new GitHub issues", "when a webhook fires, route
the payload to the research agent". The engine schedules and runs these jobs.

## Scope (v1)

- `oran-automation::Service` + `Runtime`.
- Job categories:
  - `cron` — POSIX-style cron expressions.
  - `periodic` — duration interval.
  - `triggered` — fired by external events (webhook, signal, file watch).
- Per-agent execution lease (prevent concurrent runs of same agent).
- Persistence in `automation.db` (jobs, runs, last-fired, cooldown).
- Per-category runners — built-in `heartbeat`, custom via `Category` interface.
- Hook events: `job_scheduled`, `job_started`, `job_finished`, `job_failed`.
- Notifier callback routes job output to cli / channel / web.

## Scope (v1.1)

- Per-job priority (urgent / normal / background).
- Backpressure: queue dropping with `job_dropped` hook.
- Job dependencies (A must complete before B fires).
- Conditional jobs (run only if predicate holds).

## Scope (v2)

- Distributed scheduling across runtimes (federation).
- Calendar-aware schedules (skip holidays, time-zone aware).

## Out Of Scope

- A full workflow engine (no DAGs of jobs); each job is independent.
- UI-based job builder; jobs configured via JSON or by a tool call (`automation.schedule`).

## Acceptance Criteria

1. A cron job ("`* * * * *`") fires exactly once per minute under nominal load.
2. A periodic job (every 15 s) fires within ±100 ms of the scheduled time.
3. A triggered job fires within 50 ms of the trigger event.
4. Per-agent lease prevents two concurrent runs of the same agent_key; the queued
   firing is held or dropped per policy.
5. A failing job is recorded with the failure reason; the next firing happens on
   schedule.
6. Cancelling a job mid-run respects the executor's cancellation semantics; the run
   is recorded as `aborted`.
7. `tests/automation/` ≥ 80% coverage.
8. `bench/automation/scheduler-tick` reports < 5 ms for 1 000 jobs.

## Design Doc Cross-References

- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  (cross-cutting concerns)
- [`../design-docs/async-model.md`](../design-docs/async-model.md)
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)

## Risks

- Cron-parser ambiguity (5-field vs. 6-field with seconds) — pick 5-field POSIX, add
  `seconds` as a separate optional config knob.
- Drift over long uptimes — use `oran-async`'s steady_timer with absolute next-fire
  time, not relative sleeps.

## Validation

```sh
xmake build oran-automation
xmake test test-automation
scripts/bench-compare.sh automation
```
