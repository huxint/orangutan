# Error Handling

The project's single error model is **`std::expected<T, core::Error>`**, aliased as
`core::Result<T>`. This file documents the rule and the patterns that follow from it.

## The Type

```cpp
// include/oran/core/error.hpp
namespace orangutan::core {

enum class ErrorKind {
  ok,
  cancelled,
  invalid_argument,
  not_found,
  permission_denied,
  capability_not_granted,
  config,
  auth,
  network,
  rate_limit,
  upstream,
  parsing,
  timeout,
  conflict,
  storage,
  hook_timeout,
  hook_failed,
  mailbox_overflowed,
  internal,
};

class Error {
 public:
  Error(ErrorKind, std::string message);

  ErrorKind                kind()    const noexcept;
  std::string_view         message() const noexcept;
  std::span<const std::pair<std::string, std::string>> context() const noexcept;

  // Builders for common categories.
  static Error cancelled();
  static Error invalid_argument(std::string);
  static Error network(std::string);
  static Error timeout(std::chrono::milliseconds);
  // ... etc.

  // Fluent attachment of structured context.
  Error& with(std::string key, std::string value);
  Error& with_retry_after(std::chrono::milliseconds);

  // Predicates.
  bool retryable() const noexcept;
  bool transient() const noexcept;
};

template <typename T>
using Result = std::expected<T, Error>;

}  // namespace orangutan::core
```

## Usage

```cpp
core::Result<provider::Response>
some_caller(...) {
  auto r = co_await provider.send(req);
  if (!r) return std::unexpected(r.error());

  // success path
  return *r;
}
```

When you have multiple sub-results to combine, use the legacy `utils::all_ok` pattern:

```cpp
core::Result<std::tuple<A, B, C>>
parse_three(...) {
  return core::all_ok(
      parse_a(...),
      parse_b(...),
      parse_c(...));
  // short-circuits on first error, returns the tuple on full success
}
```

`all_ok` will be reimplemented in `oran-core` from the legacy `utils::all_ok`.

## Rules

### E1. Public APIs return `Result<T>`, not throw

Library boundaries return `Result<T>`. Internal helpers may throw if local catch is
trivial; cross-library calls always go through `Result<T>`.

### E2. No exceptions across library boundaries

```cpp
// BAD: library exposes a throwing function
namespace oran::foo {
  Bar parse(std::string_view);  // throws ParseError
}

// GOOD: returns Result
namespace oran::foo {
  core::Result<Bar> parse(std::string_view);
}
```

### E3. `asio` exceptions are caught at the awaitable boundary

asio can throw `system_error` from the executor. Wrap with `try/catch` at the
public function boundary and translate:

```cpp
async::Awaitable<core::Result<Response>>
HttpClient::request(Request req) {
  try {
    co_return co_await do_request(std::move(req));
  } catch (const std::system_error& e) {
    co_return std::unexpected(core::Error::network(e.what()));
  }
}
```

### E4. No `std::exception_ptr` in interfaces

If you find yourself reaching for `exception_ptr`, you're trying to smuggle an
exception through a value-based interface — refactor to `Result<T>`.

### E5. Errors carry context

```cpp
core::Result<Response> r = co_await provider.send(req);
if (!r) {
  return std::unexpected(
      r.error()
          .with("agent", identity.agent_key)
          .with("model",  route.primary.model)
          .with("attempt", std::to_string(attempt)));
}
```

Context is what makes error logs useful. The legacy code's "the model failed" without
identity / route was a recurring debugging pain.

### E6. Retryability is a predicate on the error, not a string match

```cpp
if (err.retryable()) {
  co_await async::sleep_for(executor, backoff);
  // retry
} else {
  co_return std::unexpected(err);
}
```

`retryable()` is set on construction; do not parse the message.

### E7. Don't conflate "not found" with "error"

If your function is "look this up, may not exist", return `Result<std::optional<T>>`:

```cpp
core::Result<std::optional<memory::Record>>
MemoryRuntime::find(std::string_view id) const;
```

Sentinel-less. `Result<std::optional<T>>` makes "absent" a normal outcome and reserves
`Error` for real failures (storage broken, permission denied, …).

### E8. Logging on the error path

```cpp
auto r = co_await some_call();
if (!r) {
  log::warn("some_call failed", field("err", r.error()));
  co_return std::unexpected(r.error());
}
```

`oran::log::field("err", error)` formats kind + message + context. Don't reach for
`spdlog::error` directly; the shim handles structured fields.

### E9. Don't construct cross-cutting macros

`UNWRAP(...)`, `TRY(...)`, etc. are forbidden (critical rule C1). The explicit
`if (!r) return std::unexpected(...)` is fine; we don't optimize for keystroke count.

If a single function has > 5 such checks, that's a code-smell hint to split it.

### E10. `assert` vs. error

- `assert` for invariants — things the *programmer* must keep true.
- `Error` for things the *world* might do wrong (network down, file missing,
  user input bad).
- `OR_VERIFY(cond)` is a small helper in `oran-core` that returns
  `Error::internal("invariant violated: ...")` instead of aborting; use it where a
  process abort is too aggressive (e.g., the agent loop).

## Common Patterns

### Map A Result

```cpp
auto r = co_await provider.send(req);
auto mapped = r.transform([](Response x) { return x.usage.total_tokens; });
// mapped : Result<std::uint64_t>
```

### Recover With A Fallback

```cpp
auto first = co_await primary.send(req);
auto resolved = first.or_else([&](core::Error e) -> core::Result<Response> {
  if (!e.retryable()) return std::unexpected(e);
  return std::move(co_await secondary.send(req));  // awaited above
});
```

### Convert Optional To Result

```cpp
core::Result<Record> get_or_fail(std::optional<Record> o) {
  return o ? core::Result<Record>{*std::move(o)}
           : std::unexpected(core::Error{ErrorKind::not_found, "missing"});
}
```

## Tests

- Every public API has at least one "fails as expected" test, exercising one
  representative error category.
- `tests/<lib>/errors.cpp` is the conventional location.
- Catch2's `REQUIRE(!r);` plus `REQUIRE(r.error().kind() == ErrorKind::...)` is the
  default shape.

## Anti-Patterns

- Throwing from a function that returns `Result<T>`. Pick one.
- Returning `Result<void>` *and* logging-on-error inside the function. The caller
  decides whether to log.
- Stuffing rich context into the message string (`"timeout after 3000ms talking to
  anthropic on attempt 2"`). Use structured context fields.
- Swallowing errors silently (`(void)r;`). Either handle or propagate.

## See Also

- [`critical-rules.md#C3`](critical-rules.md) — the no-throw rule.
- [`async-and-concurrency.md`](async-and-concurrency.md) — error semantics across
  coroutines.
- [`../design-docs/api-portability.md`](../design-docs/api-portability.md) — provider
  error categories.
