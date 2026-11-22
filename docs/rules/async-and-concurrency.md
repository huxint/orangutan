# Async Rules

Use Asio awaitables and `async::Runtime`. Effectful public operations carry
`Result<T>` directly or with terminal metadata under the
[error contract](error-handling.md); translate exceptions at library boundaries.

- Give each mutable coordinator a strand. Independent values can be processed
  on worker executors without sharing mutable session state.
- Run blocking filesystem, SQLite and HTTP work on the worker executor. Awaiting
  a pool lease or posting a continuation does not migrate an entire coroutine;
  spawn the operation on its target executor and await it.
- Bound queues and owned child work. Admission failure must be observable.
- Propagate cancellation through awaitables. Cleanup may disable cancellation
  only to finish releasing resources and recording the resulting state.
- Join every child that borrows services before releasing those services.
  Cancellation and bounded-return deadlines do not prove lifetime completion.
- Pass values across suspension boundaries unless the caller explicitly retains
  the referenced object until the operation finishes.
- Destructors release synchronous resources. Async owners expose a join/shutdown
  boundary that callers await before destruction.
- Use timers and explicit test synchronization, not blocking sleeps.

[async-model](../design-docs/async-model.md) owns runtime topology and scheduler
cancellation semantics. Tests may create executors directly to exercise ownership.
