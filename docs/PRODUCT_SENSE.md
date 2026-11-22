# Product Direction

Orangutan provides a reusable agent runtime. The first milestone is a reliable
agent turn using real tools and persistent memory. Bounded child execution
reuses the same contracts with constrained authority.

Priorities, in order:

1. Complete a model → tool → model interaction with an observable result.
2. Enforce permissions on external effects; use scoped functional boundaries for
   memory and child coordination without routine approval prompts.
3. Preserve scoped context and make recall, failures and cancellation predictable.
4. Keep values and pure transformations separate from effectful execution.
5. Add a capability only when a concrete core use case requires it.

Keep one implementation per boundary. Delete replaced callers, unused settings,
speculative extension layers and their documentation in the same change. Preserve
user data and useful regression coverage. Git owns implementation history.

Application surfaces build on the [agent contract](design-docs/agent-platform.md).
[Live debt](exec-plans/tech-debt-tracker.md) records concrete reliability gaps.
