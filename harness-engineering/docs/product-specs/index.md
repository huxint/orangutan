# Product Specs Index

Product specs describe **what the user gets** at each milestone. They are paired with
design docs (which describe *how*) and execution plans (which describe *the steps to
ship*).

| ID | Spec | Status |
| -- | ---- | ------ |
| 0001 | [Core ReAct loop](0001-core-react-loop.md) | drafted |
| 0002 | [Tool registry + built-in tools](0002-tool-registry.md) | drafted |
| 0003 | [Multi-platform channels](0003-multi-platform-channels.md) | drafted |
| 0004 | [Agent team collaboration](0004-agent-team.md) | drafted |
| 0005 | [Memory system](0005-memory-system.md) | drafted |
| 0006 | [Automation engine](0006-automation.md) | drafted |
| 0007 | [Web UI](0007-web-ui.md) | drafted |
| 0008 | [Permissions engine](0008-permissions.md) | drafted |
| 0009 | [Skills](0009-skills.md) | drafted |
| 0010 | [Benchmark harness](0010-benchmark-harness.md) | drafted |

## Conventions

- One spec per feature or workflow.
- Start with the **user problem**, not the implementation.
- State **acceptance criteria** as observable outcomes.
- Cross-link to the matching design doc, execution plan, and release notes.
- Specs may be split into v1 / v1.x / v2 sections when the work is staged.
- A spec stays "drafted" until at least one acceptance criterion has shipping evidence
  (history entry + release note + test).
