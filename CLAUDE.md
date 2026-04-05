# Orangutan Project

A C++23+ agent assistant for external communication. Follow these guidelines to produce **high-quality, performant, and extensible** code.

## Available Libraries

cli11, nlohmann_json, spdlog, libcurl, sqlite3, cpp-httplib, stdexec, rapidhash, replxx, mbedtls, simdutf, uni_algo, ctre, magic_enum

---

## Code Quality

- Strictly follow `.clang-tidy` rules
- Respect all LSP diagnostics and warnings

## Workflow

- Create a new branch before making changes
- Commit incrementally per module or logical step—avoid large bulk commits
- Only compile when changes likely require it (large projects are slow to build)
- Skip over-analysis for trivially correct changes

---

## Design Principles

| Principle | Guideline |
|-----------|-----------|
| **Integration** | Design modules for seamless integration with existing architecture |
| **Composability** | Favor composable, chainable APIs |
| **Sender/Receiver** | Leverage `stdexec` patterns where appropriate |
| **Thread Pool** | Use `stdexec`'s thread pool implementation over custom or std alternatives |
| **Error Handling** | Use `try_do`/`value_or` patterns to reduce branching |
| **Wrapper APIs** | Wrap third-party libraries with unfriendly interfaces |

---

## Modern C++ Idioms

### Type & Memory

| Prefer | Over | Reason |
|--------|------|--------|
| `std::string_view` | `const std::string&` | Lightweight, zero-copy |
| `std::span<T>` | `T* + size` / `const std::vector<T>&` | Bounds-safe view |
| `std::array<T, N>` | C-style arrays | Type-safe, bounds-checked |
| `std::expected<T, E>` | Exceptions / error codes | Explicit, composable |
| `std::jthread` | `std::thread` | Auto-join, stop tokens |
| PMR allocators | Default allocators | Runtime flexibility |

### Syntax & Style

| Prefer | Over | Reason |
|--------|------|--------|
| C++ casts (`static_cast`, etc.) | C-style casts | Type-safe, explicit |
| Brace initialization `{}` | Other forms | Uniform, prevents narrowing |
| `std::ranges` algorithms | Traditional loops | Composable, lazy evaluation |
| `contains()` | `find() != end()` | Clearer intent |
| Lambdas | `std::bind` | Readable, flexible |
| `std::to_underlying(e)` | `static_cast<int>(e)` | Semantic clarity |
| `std::unreachable()` | `__builtin_unreachable()` | Portable |
| Deducing `this` (C++23) | Const/ref overloads | Eliminates duplication |

### C++23 Features to Use

- `std::ranges`: `zip`, `chunk`, `slide`, `join`, `to<Container>()`
- `std::generator` for lazy sequences
- Deducing `this` for CRTP-free mixins

---

## Coding Standards

### General Rules

- **No macros**—use `constexpr`, templates, or `inline` functions
- **No header/source split**—keep declarations and definitions together in `.hpp`
- **Explicit comparisons**—write `ptr != nullptr`, not implicit bool (unless semantically obvious)
- **Single-argument constructors must be `explicit`**
- **Rule of Zero first**; when Rule of Five is needed, mark moves `noexcept`
- **Constexpr everything possible**—prefer compile-time computation
- **Cross-platform APIs**—avoid platform-specific code when portable alternatives exist
- **Factory functions**—provide factory functions where construction is complex or requires validation
- **`// NOLINT` suppression**—only use when the warning is a confirmed false positive; add a brief justification comment

### Naming & Organization

- Use meaningful names; avoid both cryptic abbreviations and excessive verbosity
- Integer types: prefer semantic aliases from `base::` namespace in `types.hpp`
- Type aliases: centralize complex types in `types.hpp` using `using`
- Use auto when the type is verbose
- **Use Doxygen-style comments. Comments must be kept in sync with the code; outdated comments are more harmful than no comments. Good naming and code structure reduce the need for comments, but necessary TODO/FIXME items should still be included**
- Do not pollute the namespace with using namespace xxx
- Namespaces: no aliases needed for ≤2 levels; alias deeper nesting locally (don't pollute global scope)
- Namespace: all lowercase, words separated by underscores (lower_case)
- Class name: CamelCase (CamelCase)
- Function name: all lowercase, underscore-separated (lower_case)
- Variable name: all lowercase, underscore-separated (lower_case)
- Enum constant: all lowercase, underscore-separated (lower_case)
- Enum type: all lowercase, underscore-separated (lower_case)
- Private member variable: suffix with underscore (_)
- Constexpr variable: all uppercase, underscore-separated (UPPER_CASE)
- Include headers in this order: associated header (if any), C++ standard library, C standard library, third-party libraries, then project headers — each group separated by a blank line

### Performance

- Place larger struct members first to minimize padding
- Minimize unnecessary copies and constructions
- Use `std::string_view` / `std::span` for function parameters

### Logging

- Use lowercase for spdlog messages (no capitalized first letters)

### Testing

- Write meaningful, robust tests
- Skip trivial tests that verify obvious behavior

---

## Utilities (`src/utils/`)

- Place reusable, extensible utilities in `src/utils/`
- **Before implementing**: check if a utility already exists—reuse, don't duplicate
- Prefer existing STL or third-party library functions over custom implementations

## Agent Tools

Modern Alternatives
- rg not grep
- fd not find
- exa not ls
- sd not sed
- pnpm not npm
Fallback to the legacy tools when not available.