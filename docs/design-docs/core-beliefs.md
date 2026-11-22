# Core Beliefs

- Values make state visible. Prefer immutable inputs and returned state changes.
- Pure policy functions precede effectful execution. Inject clocks and services
  at the boundary that needs them.
- One domain operation has one implementation. Application surfaces compose the
  runtime instead of recreating its permission, memory or tool policy.
- Resource ownership is explicit. Every accepted child operation finishes before
  borrowed services are released.
- A small complete loop is more useful than many unconnected features.
- Documents describe current contracts. Replace or delete stale claims with the
  code; Git preserves history.

[Architecture](../ARCHITECTURE.md) names boundaries;
[critical rules](../rules/critical-rules.md) defines implementation constraints.
