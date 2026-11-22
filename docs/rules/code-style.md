# Code Style

This file collects naming, formatting, and idiom rules that aren't critical-rules-bad
but make the codebase coherent. Most rules have an enforcement hook; the rest are
review conventions.

## Naming

| Kind                       | Convention            | Example                             |
| -------------------------- | --------------------- | ----------------------------------- |
| Namespace                  | `snake_case`          | `orangutan::async`                  |
| Type (class, struct, enum) | `PascalCase`          | `MailboxMessage`, `ChannelManager`  |
| Function / method          | `snake_case`          | `send_message`, `next_message`      |
| Variable                   | `snake_case`          | `agent_key`, `cancel_signal`        |
| Private member             | `snake_case_`         | `impl_`, `cache_`                   |
| Constant / constexpr       | `UPPER_SNAKE_CASE`    | `MAX_ITERATIONS`                    |
| Enum value                 | `snake_case`          | `ChannelKind::qq`, `Verdict::deny`  |
| Concept                    | `PascalCase`          | `AwaitableOf`, `MessageLike`        |
| Template parameter         | `PascalCase`          | `template <typename Sink>`          |
| File                       | `kebab-case.hpp/cpp`  | `tool-runtime.hpp`, `loop.cpp`      |

Notes:

- The `_` suffix on private members is mandatory (clang-tidy enforced).
- Acronyms in identifiers are lowercased except at PascalCase start: `JsonValue`,
  `to_json`, `HttpClient`, `parse_url`.

## Files And Layout

- One primary class per file. Helpers live as anonymous-namespace functions in the
  `.cpp` or as `_internal` files.
- Header / source pair: `foo.hpp` + `foo.cpp`. Internal headers go under
  `src/<lib>/_impl/foo.hpp` (note the `_impl` directory).
- Filename matches the primary class name in `kebab-case`.

## Include Order

```cpp
// 1. Matching header for this .cpp (if any)
#include "loop.hpp"

// 2. C++ standard library
#include <expected>
#include <string>
#include <vector>

// 3. Third-party
#include <asio.hpp>
#include <nlohmann/json.hpp>

// 4. Project public headers
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

// 5. Project private headers (same library only)
#include "_impl/runtime-impl.hpp"
```

Blank lines between groups. `#pragma once` at the top of every header; no include
guards.

## Formatting

`.clang-format` is the source of truth; key choices:

- Column limit: **120**.
- Indent: 2 spaces.
- Braces on the same line, including namespaces.
- Templates always on their own line; arguments indented.
- Pointer/reference: type-attached (`int*`, `Foo&`).
- Sort includes within groups.

The versioned `.githooks/pre-commit` hook runs `clang-format` on staged C/C++ files and
re-stages the formatted result. If a staged C/C++ file also has unstaged hunks, the hook
refuses to format it so those hunks are not accidentally committed. Manual override is rare
and documented in the PR.

## C++ Idioms

### Constructors

- Single-argument constructors are `explicit`.
- Inheritance is sparse; prefer composition and `std::variant` for polymorphism-by-data.
- Virtual destructors only on classes that are actually inherited.
- Default special members where possible (`= default`, `= delete`).

### Const Correctness

- Methods that don't mutate are `const`.
- Parameters that are not modified are `const T&` (or `T` for cheap types).
- `[[nodiscard]]` on functions whose return value carries program meaning.

### Templates And Concepts

- Constrain templates with concepts (`template <AwaitableOf<int> A>`).
- Avoid SFINAE-style enable_if in 2026; concepts are stable in GCC 16.1.
- Concept names describe a shape: `Comparable`, `Awaitable`, `JsonSerializable`.

### Variants Over Inheritance

`core::Content` is a `std::variant<Text, Thinking, ToolUse, ToolResult>`. Visit with
`std::visit(Overloaded{...}, content)`. This is the preferred polymorphism style for
data-like types.

Inheritance with virtual functions remains for true polymorphic *interfaces*
(`Channel`, `provider::Adapter`, `hook::Sink`).

### Enums

- `enum class` always; never bare `enum`.
- Stable string spelling and inverse parse come from the reflection-backed
  helpers in [`include/oran/core/enum_names.hpp`](../../include/oran/core/enum_names.hpp):
  - `core::enum_name(value)` returns the identifier (or `"unknown"` for an
    out-of-range cast).
  - `core::parse_enum<E>(text)` is the inverse, returning
    `std::optional<E>`.
  - `core::enum_values<E>()` exposes every enumerator in declaration order.
- **Call the generic helpers directly.** Do not add a per-enum forwarding
  shim — no `std::string_view to_string_view(Foo)`, no
  `std::optional<Foo> parse_foo(std::string_view)`. The shims existed in
  early slices and have all been deleted; the generic name is short
  enough at the callsite and one less place to keep in sync.
- A `std::formatter<E>` specialization that calls `core::enum_name(value)`
  is fine and encouraged: it hooks a stdlib customization point that
  callers cannot bypass, so it's not a forwarding shim in the same sense.
- A trailing underscore on an enumerator name (used to dodge a C++
  keyword, e.g. `Mode::default_`) is stripped from the wire spelling. If
  your enum's wire format deviates from the identifier in any other way
  (dashes, alternate casing) — e.g. `bootstrap::ConfigSource` →
  `"built-in-defaults"` — the helper cannot produce it; keep a
  hand-written switch in the matching `.cpp` and document why.
- `enum_names.hpp` is a heavy include (pulls `<meta>`). Include it from
  the enum's own public header or a `.cpp` that consumes the helper —
  not from unrelated public headers.

```cpp
// PREFERRED — generic helper at the callsite.
const auto text = core::enum_name(role);
const auto parsed = core::parse_enum<core::Role>(input);

// FORBIDDEN — per-enum forwarding shim.
namespace orangutan::core {
std::string_view to_string_view(Role r) noexcept { return enum_name(r); }
std::optional<Role> parse_role(std::string_view t) noexcept {
  return parse_enum<Role>(t);
}
}  // namespace orangutan::core
```

### Strings

- `std::string` for ownership, `std::string_view` for non-owning views.
- UTF-8 by contract. Validation at boundaries via `oran-core::str::validate_utf8`.
- No `wchar_t` / `wstring` / `char*` in public APIs.

### Console And Formatted Output

We are on C++26 + GCC 16.1. **Use `std::print` / `std::println` / `std::format`**;
do **not** include `<iostream>` in production code.

```cpp
// PREFERRED
#include <print>
std::println("loaded {} routes in {}", routes.size(), elapsed);

// FORBIDDEN (in runtime libraries)
#include <iostream>
std::cout << "loaded " << routes.size() << " routes\n";
```

Rationale:

- `std::print` is type-checked at compile time via `std::format_string<...>`.
- It avoids `std::ios_base::Init` initialization cost in every TU that pulls
  `<iostream>`.
- It composes with `std::format_to(std::back_inserter(buf), ...)` when we need to
  capture rather than write directly.

`<iostream>` is permitted inside `tests/` / `bench/` *runners* (Catch2 / nanobench
do not yet emit via `std::print`); even there, prefer `std::print` for direct output.

### Numbers

- `std::uint32_t` / `std::int64_t` / etc. — never bare `unsigned long`.
- `std::size_t` for sizes/indices.
- `std::chrono::duration` and `core::Time` for time.

### Result / Optional

- `core::Result<T>` (`std::expected<T, core::Error>`) for fallible operations.
- `std::optional<T>` for "maybe absent, not an error" — typically configuration fields.
- Don't return `std::optional<core::Result<T>>` — collapse the nesting; use a richer
  `Error::not_found`.

### Algorithms And Ranges

Before writing a hand-rolled loop, search the standard library and the in-repo
helpers for a function that already does it. The win is twofold: the callsite
becomes a single named operation, and the reader does not have to verify the
loop's correctness.

- **Prefer `std::ranges::*` over the iterator-pair `std::*` algorithms.**
  `std::ranges::find_last_if(rng, pred)` is clearer than a hand-written
  reverse-iterator loop, and `std::ranges::sort(rng)` is clearer than
  `std::sort(rng.begin(), rng.end())`. Range projections (`std::ranges::sort(rng,
  {}, &Item::priority)`) eliminate temporary lambdas.
- **Reach for the stdlib first.** Check `<algorithm>`, `<ranges>`, `<numeric>`,
  `<bit>`, `<charconv>`, `<chrono>`, `<format>` before adding a private helper.
- **Reach for an in-repo lib next.** `oran-core`, `oran-async`, `oran-io`,
  `oran-storage`, … each own a slice of behavior; if your helper would fit one
  of them, add it there instead of duplicating it in the consumer.
- **Pick the newer of two equivalents.** When the standard ships both an older
  and newer facility for the same job (`std::format` vs. `sprintf`,
  `std::filesystem::path` vs. raw strings, `std::span` vs. pointer + length,
  `std::optional` vs. sentinel values, `std::variant` vs. tagged unions,
  `std::ranges::*` vs. iterator-pair `std::*`), use the newer one unless there is
  a benchmarked reason not to.

```cpp
// PREFERRED — range form, named projection, clear intent.
auto victim = std::ranges::find_last_if(cache.lru,
                                        [](const auto& e) { return !e->leased; });

// FORBIDDEN — hand-rolled loop where a one-liner exists.
auto victim = cache.lru.end();
for (auto it = cache.lru.begin(); it != cache.lru.end(); ++it) {
  if (!(*it)->leased) {
    victim = it;
  }
}
```

This rule lives alongside `critical-rules.md#C17` ("Language standard is C++26;
modern facilities preferred"). C17 covers *which* C++26 facilities to adopt;
this section covers *which* one to choose when the language offers a choice.

### Membership Tests: contains, not find != end

The standard adds first-class membership tests in C++20/23 — use them.
The pre-existing `find` form discards the iterator anyway in a
membership test; spelling it as `contains` says what the code does.

- `std::ranges::contains(rng, value)` for any range.
- `set.contains(key)`, `map.contains(key)`, `unordered_set.contains(key)`,
  `unordered_map.contains(key)` — the associative containers all have it.
- `str.contains(sub)` / `str.contains(ch)` for `std::string` and
  `std::string_view`.

```cpp
// PREFERRED
if (std::ranges::contains(kRecognizedFields, key)) { … }
if (map.contains(name)) { … }
REQUIRE(rendered.contains("network: HTTP 503"));

// FORBIDDEN — escape-past-end membership test.
if (std::ranges::find(kRecognizedFields, key) != kRecognizedFields.end()) { … }
if (map.find(name) != map.end()) { … }
REQUIRE(rendered.find("network: HTTP 503") != std::string::npos);
```

The iterator-form `find` stays appropriate when the callsite *uses* the
iterator after the check (`if (auto it = map.find(k); it != map.end()) {
return it->second; }`). `contains` discards the iterator, so a pure
membership rewrite at that callsite would force a second lookup.

This rule lives alongside the "newer of two equivalents" guidance above
and `critical-rules.md#C17`. Reviews flag new `find(…) != … .end()` /
`find(…) != … .npos` in code-change PRs unless they store and use the
iterator.

### Lambdas

- Trailing return type only when needed.
- Capture lists explicit (`[this]`, `[&counter]`); avoid `[=]` and `[&]`.
- Coroutine lambdas are allowed but must not capture references that outlive them.

### C++ Over C Idioms

Code should read as **simple, efficient, elegant, modern C++**. When the
language ships both a C-era and a C++-era spelling for the same job, the
C++ spelling is the one that lands in `src/oran-*/`. This subsection
extends [`critical-rules.md#C17`](critical-rules.md) with concrete pairs.

| C-era spelling                       | C++ replacement                                 | Notes |
| ------------------------------------ | ----------------------------------------------- | ----- |
| `(Foo)x` (C-style cast)              | `static_cast<Foo>(x)` / `reinterpret_cast<Foo*>(p)` / `std::bit_cast<Foo>(bits)` | C-style casts silently combine `static`, `const`, and `reinterpret` semantics; the named cast says which one is meant. |
| `(void)expr` (discard)               | `static_cast<void>(expr)`                       | Documented preference; clang-tidy's `cppcoreguidelines-pro-type-cstyle-cast` reports the C form. |
| `NULL` / bare `0` for pointers       | `nullptr`                                       | Type-safe; participates in overload resolution. |
| `typedef T U;`                       | `using U = T;`                                  | Template-friendly; reads left-to-right. |
| bare `enum Foo { ... };`             | `enum class Foo { ... };` + `core::enum_name`   | See "Enums" above. |
| raw `T arr[N]` in interfaces         | `std::array<T, N>` (owning) / `std::span<const T>` (view) | The raw-array form decays to a pointer at the boundary; the C++ forms keep the size in the type. |
| `malloc` / `free`                    | RAII container (`std::vector`, `std::unique_ptr`) | Manual `new` / `delete` is also discouraged outside RAII helpers. |
| `char*` / `strlen` in interfaces     | `std::string` (owning) / `std::string_view` (view) | Public APIs never expose raw `char*` — see "Strings" above. |
| `printf` / `sprintf` / `fprintf`     | `std::print` / `std::println` / `std::format`   | See "Console And Formatted Output" above. |
| `<cstring>` `memcpy` / `memcmp` / `memset` on objects | `std::copy_n` / `std::ranges::equal` / `std::ranges::fill` | The byte-level functions are fine *inside* a typed helper (e.g. a serializer); they must not be the surface a caller sees. |
| `errno` / negative-int error codes   | `core::Result<T>` = `std::expected<T, Error>`    | See [`error-handling.md`](error-handling.md). |
| `assert(cond)`                       | `[[assume(cond)]]` (intent) / `core::Error` (recoverable) | Naked `assert` evaporates in release builds; if the condition is load-bearing, return an error. |

```cpp
// PREFERRED — simple, efficient, elegant, modern.
auto buffer = std::array<std::byte, 64>{};
const auto written = std::format_to(buffer.data(), "value={}", value);
static_cast<void>(written);  // intentional discard

// FORBIDDEN — C-era spelling carried in.
char buffer[64];
int written = sprintf(buffer, "value=%d", (int)value);
(void)written;
```

The pairs above are the most common offenders. The general principle:
*reach for the C++ vocabulary first; drop to C only at typed boundaries
(serialization, FFI, OS syscalls) and wrap it immediately.*

## Logging

Hosts own diagnostic presentation. Follow [error handling](error-handling.md)
for structured error context and reporting; runtime libraries have no logging
facade. Never include secret values in diagnostics.

## Comments

Default to writing no comments. A comment is justified when:

- A non-obvious *why* is hidden by the code (an external constraint, a subtle invariant).
- A workaround for a known bug ("GCC 16.1 ICE on <issue>, https://gcc.gnu.org/bugzilla/...").

Don't comment what the code says. Don't reference tasks ("// added for ticket X").

## Doxygen-Style Public Headers (Light Touch)

Public headers get a short `///` summary per class/function — one to three lines. No
`@param`/`@return`; the signature is the source of truth. Examples in
`docs/design-docs/*.md` show the style.

## Error Construction

```cpp
return std::unexpected(core::Error::network("HTTP 503 from anthropic")
                          .with_retry_after(std::chrono::seconds(2)));
```

`core::Error` is a struct with a category enum and a payload. Never throw a raw string;
always go through the builders.

## Anti-Patterns

- "Helper" classes whose entire body is `void run()` (write a free function).
- Output parameters (`bool parse(string_view in, Foo& out)`). Return `Result<Foo>`.
- Mutable singletons.
- `using namespace` in headers (allowed in `.cpp` for `using namespace std::chrono`
  inside small functions).
- Macros for boilerplate.

## Performance Notes

- Reserve `std::vector` when the size is known.
- Use `std::span<const T>` for non-owning view parameters of contiguous data.
- Use `std::string_view` for non-owning string parameters.
- Prefer `std::pmr::polymorphic_allocator` where measurement justifies (rare).
- Don't optimize prematurely; if a bench shows a hotspot, fix that hotspot.

## See Also

- `docs/rules/critical-rules.md`
- `docs/rules/compile-budget.md`
- `docs/rules/error-handling.md`
- `.clang-format`, `.clang-tidy` (project root)
