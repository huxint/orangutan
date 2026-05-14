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
- Braces on the same line for everything except namespaces (open brace on same line).
- Templates always on their own line; arguments indented.
- Pointer/reference: type-attached (`int*`, `Foo&`).
- Sort includes within groups.

Pre-commit hook runs `clang-format` on staged files. Manual override is rare and
documented in the PR.

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
- Stringify via `oran::core::enum_name(value)` (wraps `magic_enum`).
- For hot enums, define a manual `enum_name` overload to avoid magic_enum's compile
  cost on common iteration paths.

### Strings

- `std::string` for ownership, `std::string_view` for non-owning views.
- UTF-8 by contract. Validation at boundaries via `oran-core::str::validate_utf8`.
- No `wchar_t` / `wstring` / `char*` in public APIs.

### Numbers

- `std::uint32_t` / `std::int64_t` / etc. — never bare `unsigned long`.
- `std::size_t` for sizes/indices.
- `std::chrono::duration` and `core::Time` for time.

### Result / Optional

- `core::Result<T>` (`std::expected<T, core::Error>`) for fallible operations.
- `std::optional<T>` for "maybe absent, not an error" — typically configuration fields.
- Don't return `std::optional<core::Result<T>>` — collapse the nesting; use a richer
  `Error::not_found`.

### Lambdas

- Trailing return type only when needed.
- Capture lists explicit (`[this]`, `[&counter]`); avoid `[=]` and `[&]`.
- Coroutine lambdas are allowed but must not capture references that outlive them.

## Logging

- Use the `oran::log::*` shim, never `spdlog::*` directly.
- Levels: `trace, debug, info, warn, error`.
- Structured fields preferred over composed strings: `log::info("tool dispatched",
  field("tool", name), field("ms", duration_ms))`.
- Never log raw secret values; the shim redacts but the rule prevents accidental
  leakage.

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
