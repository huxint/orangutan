# Orangutan Project — C++23 Agent Assistant Guidelines

You are an expert C++23 developer building the **Orangutan Project** — a high‑performance, extensible agent assistant. Follow this specification to produce code that is **correct, maintainable, and performant**.

## Environment & Available Libraries

The project uses the following libraries (already available, do not re‑implement their functionality):

| Category | Libraries |
|----------|-----------|
| CLI parsing | `cli11` |
| JSON | `nlohmann_json` |
| Logging | `spdlog` |
| HTTP client | `libcurl`, `cpp-httplib` |
| Database | `sqlite3` |
| Concurrency / Senders | `stdexec` |
| Hashing | `rapidhash` |
| Line editing | `replxx` |
| TLS / Crypto | `mbedtls` |
| Unicode / Text | `simdutf`, `uni_algo` |
| Compile‑time regex | `ctre` |
| Reflection utilities | `magic_enum` |

## Workflow – *Always Follow This Order*

1. **Create a new branch** before making any change.
2. **Commit incrementally** — one logical change per commit. Avoid large, mixed commits.
3. **Compile only when necessary.** The project is large and builds slowly; trust your code inspection for trivial, obviously correct changes.
4. **Do not over‑analyze trivial changes.** Focus on correctness and clarity.
5. **Keep this document updated** when important style or rule changes occur.

## Critical Rules – Do Not Violate

- **No macros.** Use `constexpr`, templates, or `inline` functions instead.
- **No implicit bool conversions.** Write `ptr != nullptr` (explicit comparisons).
- **Single‑argument constructors must be `explicit`.**
- **Follow `.clang-tidy` and LSP diagnostics** — treat all warnings as errors.
- **Do not pollute the global namespace** with `using namespace`. Use namespace aliases locally if depth exceeds 2 levels.

## Design Principles – How to Structure Code

| Principle | Implementation Requirement |
|-----------|---------------------------|
| **Integration** | Design every module to drop into the existing architecture without friction. |
| **Composability** | Provide chainable APIs that can be combined without side‑effects. |
| **Sender/Receiver** | Use `stdexec` patterns for asynchronous operations. Do not roll custom async primitives. |
| **Thread Pool** | **Always** use `stdexec`'s built‑in thread pool. Do not use `std::thread` or custom pools. |
| **Error Handling** | Use `try_do` / `value_or` patterns to flatten control flow. Avoid deeply nested `if` / `catch` blocks. |
| **Wrapper APIs** | Wrap third‑party libraries that have unfriendly interfaces. Provide a clean, C++23‑style API on top. |

## Modern C++ Idioms – *Prefer the Left Side*

### Type & Memory Choices

| ✅ **Prefer** | ❌ **Avoid** | **Reason** |
|------------|----------|---------|
| `std::string_view` | `const std::string&` | Zero‑copy, avoids allocation when passing literals / substrings. |
| `std::span<T>` | `T* + size` or `const std::vector<T>&` | Bounds‑safe view over contiguous data. |
| `std::array<T, N>` | C‑style arrays | Type‑safe, size is part of the type. |
| `std::expected<T, E>` | Exceptions or `std::error_code` | Explicit, composable error handling without hidden control flow. |
| `std::jthread` | `std::thread` | Automatic joining and stop‑token support. |
| `std::scoped_lock` | `std::lock_guard` | C++17's `scoped_lock` handles multiple mutexes and deduces template arguments. |
| PMR allocators | Default allocators | Enable runtime memory strategy tuning without changing container types. |

#### `std::string_view` vs `const std::string&` – Know the Difference

| Scenario | `const std::string&` | `std::string_view` |
|----------|---------------------|-------------------|
| Passing a string literal | **Allocates + copies (expensive)** | **Zero overhead** |
| Passing an existing `std::string` | Cheap (no copy) | Cheap (no copy) |
| Need null‑terminated string | **Yes** (`.c_str()` available) | **No guarantee** — do not pass to C APIs expecting `\0`. |
| Original string may be modified | Safe (if lifetime managed) | **Dangerous** — view may dangle. |

**Rule:** Use `std::string_view` for read‑only inspection. Use `const std::string&` only when the callee requires null termination or stores the string.

### Syntax & Style Choices

| ✅ **Write This** | ❌ **Not This** | **Why** |
|----------------|-------------|------|
| `static_cast<int>(x)` | `(int)x` | Type‑safe, greppable, explicit intent. |
| `int x{42};` | `int x = 42;` | Uniform initialization prevents narrowing conversions. |
| `std::ranges::sort(v);` | `std::sort(v.begin(), v.end());` | Cleaner, composable, works directly on containers. |
| `if (map.contains(key))` | `if (map.find(key) != map.end())` | Clearer intent, less noise. |
| Lambda `[&](int x) { ... }` | `std::bind(...)` | Readable, captures are explicit. |
| `std::to_underlying(e)` | `static_cast<int>(e)` | Semantic: "I want the underlying value". |
| `std::unreachable()` | `__builtin_unreachable()` | Portable and standard. |
| Deducing `this` (C++23) | Separate const/non‑const overloads | Eliminates code duplication for member functions. |

### C++23 Features – Use These Actively

- `std::ranges::to<Container>()` – Convert views to concrete containers.
- `std::views::zip`, `chunk`, `slide`, `join` – Compose data transformations without loops.
- `std::generator<T>` – Implement lazy, coroutine‑based sequences.
- Deducing `this` – Replace CRTP with self‑explicit object parameters.

## Naming & Code Organization – *Strict Conventions*

| Entity | Convention | Example |
|--------|------------|---------|
| Namespace | `snake_case` | `orangutan::network` |
| Class / Struct | `CamelCase` | `HttpClient` |
| Function | `snake_case` | `send_request` |
| Variable | `snake_case` | `request_count` |
| Enum type | `snake_case` | `error_code` |
| Enum values | `snake_case` | `timeout` |
| Private member variable | `snake_case_` | `socket_fd_` |
| Constexpr / compile‑time constant | `UPPER_CASE` | `MAX_RETRIES` |

### Header Include Order

Group includes with a blank line between groups:

1. Associated header (if any)
2. C++ standard library headers
3. C standard library headers (wrapped in `extern "C"` if needed)
4. Third‑party library headers
5. Project headers (`src/...`)

## Performance & Safety – *Every Line Matters*

- **Place larger struct members first** to minimize padding. Use `alignas` only when necessary.
- **Avoid unnecessary copies** – pass views, move when ownership transfers.
- **Prefer `std::string_view` and `std::span` for function parameters** unless you need to take ownership.
- **Follow the Rule of Zero.** If you must define a destructor, copy constructor, or copy assignment, define all five (Rule of Five) and mark move operations `noexcept`.
- **Mark everything `constexpr` that can be computed at compile time.**
- **Use factory functions** (`static std::expected<MyClass, Error> create(...)`) when construction requires validation or complex setup.

## Logging with `spdlog`

- Log messages **must be all lowercase** (no capital first letters).
- Example: `logger->info("connection established to {}", host);`

## Testing

- Write **meaningful, robust tests**. Do not write tests for trivially correct behavior (e.g., getter/setter pairs).
- Focus on edge cases, error paths, and integration points.

## Utility Code (`src/utils/`)

- **Before implementing a new utility**, check `src/utils/` for an existing solution. Reuse, don't duplicate.
- Prefer STL or library functions over custom implementations.

## Command‑Line Tools – *Modern Alternatives First*

When invoking external tools (e.g., for searching or file manipulation), prefer the modern equivalents. Fall back to legacy tools only if the modern version is not available.

| Task | **Use This First** | Fallback |
|------|-------------------|----------|
| Search code | `rg` | `grep` |
| Find files | `fd` | `find` |
| List directory | `exa` | `ls` |
| Replace text | `sd` | `sed` |
| Package manager | `pnpm` | `npm` |

## Comments & Documentation

- Use **Doxygen‑style comments** for public APIs (`///` or `/** */`).
- **Keep comments in sync with code.** Outdated comments are worse than no comments.
- Let **good naming and structure** reduce the need for comments. Reserve comments for *why*, not *what*.
- Include `// TODO(username): description` and `// FIXME(username): description` when necessary.

## Suppressing Lint Warnings (`// NOLINT`)

Use `// NOLINT` **only** when the warning is a confirmed false positive. **Always add a brief justification** on the same line:

```cpp
int legacy_flag = 0; // NOLINT: required by external C API
```

---

**Remember:** You have more than enough time to complete each task. Focus on quality over speed, and continue improving your implementation until it meets every requirement above.