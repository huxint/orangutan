# Async And Concurrency

The project uses **asio + C++20 coroutines** as its one async vocabulary. This file is
the rules-level companion to [`../design-docs/async-model.md`](../design-docs/async-model.md)
— the design doc explains *what* and *why*; this file enforces *how*.

## A1. One executor

`oran::async::Runtime` owns the single `asio::io_context`. Nothing else creates one.
The runtime is constructed by `oran-bootstrap` and threaded into every library by
reference (via `Services` / context objects).

**Enforcement:** review checklist. A library that constructs its own `io_context`
fails review.

## A2. All async returns `Awaitable<Result<T>>`

```cpp
async::Awaitable<core::Result<Response>> send(Request);
```

Never `std::future<T>`, never `boost::future<T>`, never raw callbacks, never
`stdexec::sender_of<...>`. The legacy code's `stdexec` shape is gone.

**Enforcement:** `scripts/check-banned-includes.sh` rejects `#include <future>` and
`#include <stdexec/...>` in `src/` and `include/`.

## A3. No `std::thread`, `std::jthread`, or custom thread pool

`asio::thread_pool` is wrapped inside `Runtime`; everywhere else uses
`runtime.executor()` or `runtime.cpu_executor()`.

**Exception:** `tests/<lib>/` may use `std::thread` for testing concurrency primitives.
This is the only allowed use.

**Enforcement:** `scripts/check-banned-includes.sh`.

## A4. Cancellation is universal

Every public `Awaitable<Result<T>>` function must either:

- Periodically check `asio::this_coro::cancellation_state`, **or**
- Be composed entirely of awaitables that are themselves cancel-aware.

A function that ignores cancellation must say so in a comment:

```cpp
// NOTE: this function is not cancel-aware because <reason>.
// Callers must complete it or rely on the runtime's hard-stop on shutdown.
```

These exceptions are reviewed individually.

**Enforcement:** review checklist + a bench in `bench/async/` that measures
cancellation latency on representative workloads.

## A5. Bounded queues by default

Use `oran::async::Channel<T>` (bounded, asio-aware). Unbounded channels require a
justification comment explaining why dropping is worse than memory growth.

```cpp
// GOOD
async::Channel<MailboxMessage> mailbox{runtime.executor(), /*capacity=*/64};

// REQUIRES JUSTIFICATION
async::UnboundedLogChannel<LogEvent> sink;
```

**Enforcement:** review checklist.

## A6. Strands for per-key serialization

Use `asio::strand` to make a coroutine "single-threaded among N threads" without
explicit mutexes:

```cpp
auto strand = runtime.make_strand();
asio::co_spawn(strand, agent.run(prompt), asio::detached);
```

The orchestration manager uses per-agent strands. The SQLite writer uses a per-DB
strand. The channel manager uses a per-conversation strand.

## A7. No blocking calls on the executor thread

- Don't call `std::this_thread::sleep_for`. Use `async::sleep_for(executor, dur)`.
- Don't make synchronous SQLite/file/network calls inside an awaitable without
  `co_await async::post(runtime.cpu_executor(), [&] { ... })`.
- Don't acquire a `std::mutex` for more than microseconds. If you need longer, the
  data should live behind a strand.

**Enforcement:** clang-tidy `bugprone-blocking-on-event-loop` (custom check; TBD).

## A8. Detached coroutines must catch and log

```cpp
asio::co_spawn(
    runtime.executor(),
    [&]() -> async::Awaitable<void> {
      try {
        co_await long_work();
      } catch (const std::exception& e) {
        log::error("detached coroutine failed", field("what", e.what()));
      }
    },
    asio::detached);
```

A detached coroutine that doesn't catch leaks the exception. The `Runtime::run()`
shutdown should still complete; an uncaught exception in a detached coroutine
terminates the io_context.

## A9. No coroutine `co_await` in destructors

```cpp
class Foo {
 public:
  // BAD: blocking shutdown
  ~Foo() {
    /* somehow run a coroutine */
  }

  // GOOD: explicit shutdown
  async::Awaitable<core::Result<void>> shutdown();
};
```

Destructors are synchronous. Provide an explicit `shutdown()` for cleanup that needs
async work; callers `co_await` it before destruction.

## A10. Coroutine lifetime: capture by value or share-pointer

Coroutine frames live on the heap (unless HALO elides). References into the caller's
stack frame go dangling at the first suspension point. Capture data by value:

```cpp
// BAD
async::Awaitable<core::Result<void>>
run(const std::string& s) {
  co_await async::sleep_for(executor_, 1s);
  log::info("got: {}", s);   // 's' may be destroyed by now
}

// GOOD
async::Awaitable<core::Result<void>>
run(std::string s) {
  co_await async::sleep_for(executor_, 1s);
  log::info("got: {}", s);
}
```

## A11. Don't recurse into the same strand

If coroutine `f` runs on strand `s`, and `f` `co_await`s a coroutine that also tries
to dispatch onto `s`, you have a deadlock.

If you need to call back onto the strand, use `asio::post(strand, ...)` so the
work is deferred to after `f` releases the strand.

## A12. No `pthread_create`, no `clone(2)`, no system thread APIs

Subprocesses are fine (the IO tool spawns them via asio). Threads are not.

**Enforcement:** `scripts/check-banned-includes.sh` rejects `<pthread.h>`.

## A13. Timer cancellation is checked

```cpp
co_await async::sleep_for(executor_, 5s);
// after this returns, check cancellation:
if (co_await asio::this_coro::cancellation_state.cancelled()
        != asio::cancellation_type::none) {
  co_return std::unexpected(core::Error::cancelled());
}
```

The pattern is wrapped in a helper:

```cpp
core::Result<void> r = co_await async::sleep_for_cancelable(executor_, 5s);
if (!r) co_return std::unexpected(r.error());
```

Use the helper.

## A14. Test asynchronicity with a real `io_context`

`tests/async/` provides:

- `run_one(awaitable)` — drives an io_context until completion; returns the awaited value.
- `run_with_timeout(awaitable, dur)` — same but with a hard timeout.
- `MockClock` — for time-dependent code.

Don't use `std::async` or `std::future` in tests; the test framework should match
production async style.

## See Also

- [`../design-docs/async-model.md`](../design-docs/async-model.md) — design doc.
- [`error-handling.md`](error-handling.md) — `Result<T>` integration with awaitables.
- [`critical-rules.md#C2`](critical-rules.md) — the no-thread rule.
- [`critical-rules.md#C11`](critical-rules.md) — cancellation rule.
