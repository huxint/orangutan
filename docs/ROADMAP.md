# Roadmap

| Step | Outcome | Acceptance |
| --- | --- | --- |
| Minimal runtime | One provider/tool/session path exposed through C++ libraries; peripheral code and configuration removed. | Build, core integration tests and repository gate pass. |
| Functional boundaries | Explicit session values, pure context/permission functions and injected effects. | State transitions test independently; composition tests exercise real effects. |
| Memory and tool reliability | Scoped recall, persistence, authorization, bounded scheduling and joined cancellation. | A continued session uses saved context; refused actions have no effect. |
| Agent collaboration | Bounded child execution with independent sessions and constrained authority. | A parent receives a child's result and cancels it safely; children cannot widen authority. |
| Agent teams | Dynamic persistent specialists, reviewed dependencies and bounded direct messages. | Corrections reuse context; only current reviewed results unlock work; delivery and cancellation preserve ownership. |

These contracts form the implemented core. [STATUS.md](STATUS.md) records the
verification and handoff; [live debt](exec-plans/tech-debt-tracker.md) scopes
remaining reliability work. Application surfaces build on these boundaries.
