# Async Ownership

Asio awaitables are the runtime's async vocabulary. Operations carry `Result<T>`
directly or with terminal metadata, following the
[error contract](../rules/error-handling.md). `async::Runtime` owns an IO executor
and a bounded worker pool. Libraries receive executor handles from their host.

## Execution

Coordinating state and hooks run on a strand. Synchronous filesystem, SQLite and
HTTP work runs on the blocking executor. Pool lease completions preserve the
requesting coroutine's executor: start the storage operation with `asio::co_spawn`
on its worker executor and await the result back on the caller's strand. Audit
and trace writers use this boundary, including terminal traces after cancellation.

Pure functions receive values and clocks explicitly. Effectful functions own or
borrow their resources for a documented lifetime. References passed through a
suspension remain valid until the awaited operation completes.

## Cancellation And Joining

`TaskGroup` owns bounded sibling work. Close admission, request cancellation,
then await `join()` before releasing services borrowed by a child. Its bounded
join variant may report lagging children; that report does not end their lifetime.

The tool scheduler returns cancellation after a 100 ms grace window, recording
lagging tools. `wait_idle(context)` subsequently joins dispatches borrowing the
selected context. The null-context form joins all dispatches. Session owners use
the selected form before destroying any turn's context, including successful
turns with timed-out tools. An unrelated session does not extend that lifetime
boundary. `AgentRun` child sessions share this scheduler with their parent. Each
child drains its own dispatch context; the parent then drains the call that owns
the child. This ordering prevents both premature destruction and a join cycle
through the waiting parent call.

Runtime shutdown follows coroutine cleanup. `stop()` alone does not join worker
threads; start-mode owners use `join()` or `stop_and_join()` as appropriate.
SIGINT/SIGTERM emits cooperative cancellation from the application strand.

## Bounds

Cross-coroutine queues use `async::Channel<T>` with explicit capacity. Admission
failure is observable. Task groups and schedulers also have finite capacity.
The tool scheduler admits at most its per-batch parallel limit as live coroutine
frames. Completion means the dispatch frame has been released before another
queued value starts. Cancellation drops queued values without dispatching or
misreporting them as laggards. Per-batch limits are independent for nested child
sessions, avoiding parent/child permit deadlocks.
Timers replace blocking sleeps. Tests use real executors and synchronization
points, with a hard timeout to expose missing completion.

See [async rules](../rules/async-and-concurrency.md) for implementation constraints.

## Work Beyond A Foreground Turn

Bootstrap's background task service owns one bounded `TaskGroup` per active
child. The service owns the shared registry/scheduler and retains each child's
policy, session and deadline until its group joins. A separate monitor publishes
terminal state only after that join. The task service itself remains alive
through its monitors; hosts must still retain borrowed provider/configuration/
assembly services until `shutdown()` completes. This does not change the shorter
lifetime of ordinary foreground dispatch contexts or synchronous AgentRun.
