# 0004 — Agent Team Collaboration

## User Problem

A single agent has bounded expertise: it has one toolset, one model, one set of
permissions. Real tasks often want a *team*: a coder + a reviewer; a researcher who
spawns sub-researchers; a leader who delegates and merges findings. The legacy
`orangutan/` had `OrchestrationManager` but no explicit coordination strategies; v2
makes teams first-class.

## Scope (v1)

- `oran-orchestration::Manager` with team lifecycle.
- `oran-orchestration::Mailbox` — bounded, idempotency-tagged.
- `Strategy` trait with two built-ins:
  - `LeaderWorker` — leader spawns workers, merges results, declares completion.
  - `Pipeline` — A → B → C linear handoff.
- Team-scoped shared memory tier (`memory-system.md`).
- Conversation DAG persisted to `orchestration.db`.
- Hook events: `team_created`, `worker_spawned`, `worker_stopped`, `team_message`,
  `team_broadcast`, `conversation_completed`, `conversation_aborted`.
- Permission overlay: per-team permission intersection with member agent permissions.

## Scope (v1.1)

- `Vote` strategy — agents answer the same prompt independently; aggregator picks.
- `RoundRobin` strategy.
- `FreeForm` strategy — pure mailbox-based; leader declares termination.
- Web UI rendering of the conversation DAG.

## Scope (v2 stretch)

- Custom strategies via registered plugin classes.
- Federated runtimes — workers on remote hosts via a typed RPC trait.
- Approval-via-channel — strategy can pause and ping a human approver.

## Out Of Scope

- Multi-tenant teams (every team in v1 belongs to one runtime).
- Worker auto-scaling.

## Acceptance Criteria

1. `agent.spawn_team(team_def, prompt)` returns within 200 ms of leader's first
   iteration starting.
2. A leader can spawn at least 4 workers concurrently on an 8-core machine without
   visible scheduling stalls.
3. Mailbox delivers at-least-once; duplicate messages with the same idempotency tag
   are dropped silently by the recipient.
4. Cancelling a team-running CLI session terminates all member loops within 5 s.
5. The conversation DAG is queryable post-hoc: given a conversation_id, the API can
   return the full node + edge list.
6. Shared-tier memory writes by one teammate are visible to others on the next read.
7. Team permission overlay is correctly applied: a member's effective permissions are
   the *intersection* of agent permissions and team overlay.
8. `bench/orchestration/team-spawn` reports < 250 ms from spawn to leader-first-token.

## Design Doc Cross-References

- [`../design-docs/team-collaboration.md`](../design-docs/team-collaboration.md)
- [`../design-docs/memory-system.md`](../design-docs/memory-system.md) (shared tier)
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)

## Risks

- Concurrency bugs in mailbox idempotency — write explicit property tests for
  ordering + dedup.
- Strategy deadlocks (leader waits for worker that waits for leader) — strategy
  must declare termination via `is_done`; CI catches the infinite-loop case via
  bounded iteration counts in tests.
- Permission overlay subtleties — table-driven tests for the intersection.

## Validation

```sh
xmake build oran-orchestration
xmake test test-orchestration
scripts/bench-compare.sh orchestration
```
