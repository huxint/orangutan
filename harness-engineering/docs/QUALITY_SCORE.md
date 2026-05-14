# Quality Score

Track quality by area so agents can prioritize the weakest parts of the system. The
table represents the *current* state of the rewrite. Update in the same PR that
materially changes an area.

## Scale

- `A` — strong coverage, stable behavior, clear docs, low operational risk.
- `B` — acceptable but has known gaps.
- `C` — works but needs targeted hardening.
- `D` — fragile, under-specified, or not yet implemented.
- `—` — not applicable (e.g., shaped only).

## Current State (Pre-v1)

| Area                     | Score | Why | Next Step |
| ------------------------ | ----- | --- | --------- |
| Architecture docs        | B     | Top-level map + design docs drafted. Some sections await concrete code. | Land MVP code; back-fill diagrams. |
| Build system             | C     | xmake design captured; not yet provisioned. | Implement skeleton + `xmake build` green. |
| Compile-time discipline  | C     | Budgets and rules captured; not yet enforced (no code). | Land `check-compile-budget.sh`. |
| Test framework           | D     | Layout defined; no real tests exist yet. | First tests land with MVP loop. |
| Bench harness            | D     | Layout + product spec defined. | First bench lands with MVP loop. |
| Async model              | C     | Design captured; needs implementation. | Implement `Runtime` + `Channel<T>`. |
| Storage / DBs            | C     | Design captured; expected-only API design done. | Implement `oran-storage` core. |
| Provider system          | C     | Layered design captured. | First adapter (Anthropic Messages). |
| Tool registry            | C     | Design captured. | First built-ins (file + shell + memory). |
| Memory tiers             | C     | Tier design captured. | Long-term FTS5 v1. |
| Permissions              | C     | Engine design captured; ctre→re2 swap planned. | Implement engine. |
| Hooks                    | C     | Enumerated lifecycle. | Shell + in-proc sinks. |
| Channels                 | C     | Trait + capability matrix designed. | QQ port + webhook adapter. |
| Orchestration / teams    | C     | Coordination strategies designed. | LeaderWorker + Pipeline. |
| Automation               | C     | Job categories designed. | Cron + periodic + triggered. |
| Web UI                   | C     | Routes + token-auth designed. | Single-page chat. |
| CLI                      | C     | REPL + single-shot defined. | First REPL. |
| Skills                   | D     | Spec drafted; no implementation. | Loader + watcher + `skill.invoke`. |
| Observability            | D     | Logging shim designed. | Metrics endpoint. |
| Security defaults        | B     | Captured. | Implement secret rotation. |
| Supply chain             | B     | Workflows pinned, lockfile present. | Add OSV scan once xmake lock is real. |

## Cadence

Reviewed at every v1.x milestone. PRs that move a row's score change it (up or down)
in the same change.

## How To Move A Row Up

- D → C: implementation exists + smoke test passes.
- C → B: full test bucket green, design doc current.
- B → A: bench coverage in place, ≥ 30 days of stable operation, no known issues
  in the tech-debt tracker for that area.
