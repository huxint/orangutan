# Async Model

Orangutan v2 has **one** async vocabulary: standalone **asio** + **C++20 coroutines**.
Everything that crosses a yield point is an `awaitable<T>`. Everything is driven by a
single `asio::io_context` wrapped in `oran::async::Runtime`. There are no
`std::thread`s, no custom thread pools, no NVIDIA `stdexec` senders.

> The legacy `orangutan/` used a custom `stdexec` fork (NVIDIA gtc-2026). That fork
> existed in five files but transitively bled into `oran::provider`, `oran::automation`,
> and channel dispatch, becoming a top-tier compile-time tax. v2 chooses asio
> deliberately for its compile-time profile and ecosystem maturity.

## Runtime Topology

```
                                              ┌──────────────────────────┐
oran::async::Runtime                          │  CPU pool (fixed)        │
 ├── asio::io_context (single instance)       │  size = config.runtime    │
 │     ├── thread #1                          │         .cpu_workers     │
 │     ├── thread #2                          │  for: memory distillation,│
 │     ├── …                                  │       large JSON parse,  │
 │     └── thread #N                          │       prompt rendering > │
 │   (size = config.runtime.workers,          │             64 KiB       │
 │    default = min(8, hardware_concurrency)) │                          │
 │                                            └──────────────────────────┘
 ├── stop_source (one shutdown signal)
 ├── steady_timer factory
 └── strand factory (per-agent serialization)
```

- **All I/O** (HTTP, SQLite via `oran-storage`'s WAL writer, file IO, subprocess) runs
  on the io_context's thread pool.
- **CPU-bound work** is dispatched to the separate `cpu_pool` via
  `co_await async::post(cpu_pool, ...)`.
- **Per-agent serialization** uses an `asio::strand` so that callbacks for the same
  agent never overlap, while different agents can run in parallel.
- **Bootstrap** instantiates exactly one `Runtime` and passes it by reference everywhere
  it's needed (see `docs/design-docs/module-boundaries.md`).

## Public Surface

```cpp
// include/oran/async/runtime.hpp — PUBLIC
namespace orangutan::async {

class Runtime {
 public:
  explicit Runtime(RuntimeConfig);
  ~Runtime();

  // Underlying executor. Library code accepts this by value (it's a handle).
  asio::any_io_executor executor() const noexcept;
  asio::any_io_executor cpu_executor() const noexcept;

  // Run until stop() is called or signal received.
  core::Result<void> run();
  void stop() noexcept;

  // Strand factory: serializes work for one agent.
  asio::strand<asio::any_io_executor> make_strand() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::async
```

The Runtime is owned by `oran-bootstrap`. Nothing else creates one.

## Awaitable Alias

```cpp
// include/oran/async/awaitable_fwd.hpp
namespace orangutan::async {

template <typename T>
using Awaitable = asio::awaitable<T>;

}  // namespace orangutan::async
```

Every async function returns `Awaitable<core::Result<T>>`. Examples:

```cpp
Awaitable<core::Result<provider::Response>> provider::System::send(...) const;
Awaitable<core::Result<tool::Output>>       tool::Registry::dispatch(...) const;
Awaitable<core::Result<memory::Records>>    memory::Runtime::recall(...) const;
```

**Awaitables compose**; do not wrap them in `std::future` or custom promise types.

## Cancellation

Every public async function is cancel-aware. The pattern:

```cpp
Awaitable<core::Result<provider::Response>> send(provider::Request req) {
  auto cancel = co_await asio::this_coro::cancellation_state;
  if (cancel.cancelled() != asio::cancellation_type::none) {
    co_return core::Error::cancelled();
  }
  // ... actual work, periodically yielding so cancellation is delivered.
}
```

Subsystems that initiate cancellation:

- `oran-bootstrap` on SIGINT/SIGTERM: signals the runtime's root cancellation_signal.
- `oran-agent::Loop` on user `/cancel` or web "stop" button: cancels the in-flight
  iteration.
- `oran-orchestration` when a worker is stopped by a leader.
- `oran-automation` when a job is unscheduled mid-run.

Cancellation semantics are documented per subsystem in
`docs/design-docs/<subsystem>.md` under "Cancellation".

## Backpressure

Bounded queues are the default. The `oran::async::Channel<T>` type wraps a typed,
bounded, asio-aware queue:

```cpp
// include/oran/async/channel.hpp
namespace orangutan::async {

template <typename T>
class Channel {
 public:
  Channel(asio::any_io_executor, std::size_t capacity);

  // Returns Awaitable<Result<void>>; resolves when the item is enqueued.
  Awaitable<core::Result<void>> send(T value);

  // Non-blocking try_send: returns OverflowError if full.
  core::Result<void> try_send(T value);

  // Returns Awaitable<Result<T>>; resolves when an item is available.
  Awaitable<core::Result<T>> receive();

  void close() noexcept;
};

}  // namespace orangutan::async
```

Used by:

- `oran-orchestration::Mailbox` — bounded per agent.
- `oran-channel::ChannelManager::inbound_queue` — bounded per channel adapter.
- `oran-automation::JobQueue` — bounded; oldest pending job is dropped with a hook
  event when overflow occurs.

Unbounded channels are allowed only for **log / metric publication**, where dropping is
worse than buffering. They live in `oran-log` and are clearly named
`UnboundedLogChannel`.

## Strands & Serialization

Use a `strand<any_io_executor>` to make a coroutine "single-threaded among multiple
threads":

```cpp
auto strand = runtime.make_strand();
asio::co_spawn(strand, agent_loop.run(prompt), asio::detached);
```

Conventions:

- **One strand per agent** for ReAct iterations. Different agents truly run in parallel.
- **One strand per session DB writer** — SQLite WAL allows concurrent readers but only
  one writer; the strand enforces this without an explicit mutex.
- **Hooks** within an agent's iteration run on the agent's strand, so the agent never
  observes a hook firing mid-iteration except at the documented checkpoints.

## Timer / Sleep

`oran::async::sleep_for(executor, duration)` returns `Awaitable<void>`; cancel-aware.
Never use `std::this_thread::sleep_for` — it blocks the executor thread.

## Detached Tasks

Detached tasks are rare and require justification. Pattern when needed:

```cpp
asio::co_spawn(
    runtime.executor(),
    [&]() -> Awaitable<void> {
      try {
        co_await long_running_work();
      } catch (const std::exception& e) {
        oran::log::error("detached task failed: {}", e.what());
      }
    },
    asio::detached);
```

The exception-to-log pattern is required because nothing else will catch them.
Detached tasks **must** be cancel-aware so shutdown completes promptly.

## Sender/Receiver Compatibility

We do **not** adopt `stdexec`. Pros of `stdexec` (composability, type-erased pipelines)
were not worth the compile-time and toolchain coupling cost in v1 and are not adopted
in v2. If someone proposes a sender-based subsystem, the answer is "wrap it behind an
`Awaitable<T>` boundary so the rest of the codebase doesn't see it." Sender/receiver
stays a private implementation detail of, at most, one library.

## Coroutine Allocation Awareness

GCC 16.1 supports the C++26 paper P2025 ("Guaranteed copy elision for return values
of coroutines") and improved HALO (heap-allocation-elision optimization). To keep
allocations rare:

- Mark inner-loop coroutines `[[nodiscard]]`.
- Pass owned data by value at the suspend boundary so escape analysis can elide.
- Avoid `std::function` on the coroutine path; prefer typed concept-bounded callbacks.

`bench/async/` ships a comparison between awaitable-based vs. callback-based
implementations of the same operation to keep us honest.

## Why Not std::async / std::thread / std::jthread?

| Concern        | std::thread / std::jthread          | asio + coroutines              |
| -------------- | ----------------------------------- | ------------------------------ |
| Compile cost   | Low                                 | Modest (asio is reasonable)    |
| Cancellation   | `jthread::stop_token` only at thread boundary | First-class, fine-grained |
| Composition    | Manual                              | `co_await` everywhere          |
| Backpressure   | DIY                                 | `Channel<T>`                   |
| Strand semantics | DIY                                | `asio::strand`                 |
| Mock for tests | Hard                                | `runtime` is a single seam     |
| Resource use   | One thread per long-running task    | Many coroutines on N threads   |

We pay a bit of compile cost (mitigated by PCH + module boundaries) for a cohesive
single-language async model that is genuinely scalable. The legacy `std::thread`-free
rule in `orangutan/` was the right call; we keep it.

## Testing Async Code

- Use `asio::io_context` directly in unit tests; do not create a `Runtime` per test.
- `tests/async/` ships helper `run_one(awaitable)` that drives the executor until the
  coroutine completes and returns the result.
- Time-dependent code uses `oran::async::MockClock` (lives in `oran-async`'s public
  surface, behind `_test` namespace).

## Pitfalls Flagged In Review

- Calling synchronous SQLite from inside a coroutine without `asio::post(cpu_executor)`
  — blocks the io thread. Use the storage library's async wrappers.
- Returning `Awaitable<T>` from a function that captures references to locals — easy
  to dangle.
- Spawning a coroutine on the wrong executor (e.g., spawning a tool on the agent's
  strand while the strand is also doing the loop — deadlock potential if the tool
  awaits a future on the same strand).
- `co_await` inside a destructor — coroutine state will leak. Make cleanup explicit.
