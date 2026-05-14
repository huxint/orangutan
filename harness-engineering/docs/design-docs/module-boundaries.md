# Module Boundaries

This document defines what each library may depend on, why those rules exist, and how to
keep translation units (TUs) small. **The compile-time savings of v2 over the legacy
code come almost entirely from boundary discipline; the rest is PCH and modules.**

> The legacy `orangutan/` packed everything into a single `orangutan-lib` static library
> (see legacy `xmake/targets.lua`) and shoved 16 third-party packages onto every TU.
> v2 splits libraries by concern, and each library only depends on what it actually
> needs.

## Dependency Direction

The canonical layering is:

```
┌─────────────────────────────────────────────────────────┐
│ Interface layer                                         │
│   oran-cli, oran-web, oran-channel*, orangutan binary   │
├─────────────────────────────────────────────────────────┤
│ Agent runtime layer                                     │
│   oran-agent, oran-orchestration, oran-automation       │
├─────────────────────────────────────────────────────────┤
│ Composition utilities                                   │
│   oran-prompt, oran-tool, oran-memory, oran-permission, │
│   oran-hook, oran-skill, oran-provider                  │
├─────────────────────────────────────────────────────────┤
│ Platform layer                                          │
│   oran-async, oran-http, oran-io, oran-storage,         │
│   oran-config, oran-log                                 │
├─────────────────────────────────────────────────────────┤
│ Foundation                                              │
│   oran-core (types, Result<T>, Error, str, time)        │
├─────────────────────────────────────────────────────────┤
│ stdlib + approved 3rd-party                             │
└─────────────────────────────────────────────────────────┘
```

**Rule**: a library may depend only on libraries strictly below itself in the diagram.
Sibling libraries inside the same layer are mutually exclusive unless an explicit
dependency is listed in [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md#library-inventory).

CI script `scripts/check-deps.sh` (in the build skeleton) walks each library's
`xmake.lua` and rejects PRs that introduce upward or sideways deps.

## Public vs. Private API

Every library has:

- **`include/oran/<lib>/*.hpp`** — the public surface. Forward-declaration-heavy.
  Maximum **6 transitive standard-library headers** per public header (stdlib only,
  enforced via `scripts/check-includes.sh`).
- **`src/<lib>/*.hpp`** and **`src/<lib>/*.cpp`** — implementation. Heavy includes
  (`nlohmann/json.hpp`, `httplib.h`, `asio.hpp`, `sqlite3.h`) live here only.

Anything in `include/oran/<lib>/` that needs a 3rd-party type holds it by **pointer or
forward-declared reference**, never by value. Example:

```cpp
// include/oran/agent/loop.hpp — PUBLIC
#pragma once
#include <memory>
#include <string>
#include <oran/core/result.hpp>          // light header, OK
#include <oran/async/awaitable_fwd.hpp>  // forward-decl shim

namespace orangutan::provider { class System; }
namespace orangutan::tool     { class Registry; }
namespace orangutan::memory   { class Runtime; }

namespace orangutan::agent {

struct LoopConfig {
  std::string agent_key;
  // ...
};

class Loop {
 public:
  Loop(provider::System&, tool::Registry&, memory::Runtime&, LoopConfig);
  ~Loop();
  Loop(Loop&&) noexcept;
  Loop& operator=(Loop&&) noexcept;

  async::Awaitable<core::Result<RunResult>> run(std::string prompt);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;  // pimpl: kills transitive include cost
};

}  // namespace orangutan::agent
```

Notice: no `<asio.hpp>`, no `<nlohmann/json.hpp>`, no `<spdlog/spdlog.h>` in the public
header. `Loop`'s public surface is roughly **20 lines** — the implementation can be
hundreds.

## Pimpl Discipline

Use `std::unique_ptr<Impl>` (the **pimpl idiom**) whenever a class's private members
would otherwise force public includes of heavy types. This is not optional for any
class that:

- Owns an asio object (e.g., `io_context`, `cancellation_signal`).
- Owns a JSON object.
- Owns an SQLite handle.
- Owns a libcurl handle.

Rule of thumb: if removing `impl_->` from the class makes the header pull in a header
you don't want to pay for, the pimpl is justified.

For `oran-core` types (`Result<T>`, `Error`, `Message`, `Content`) we deliberately keep
them inline / non-pimpl because they appear in every signature and the included headers
are stdlib-only.

## One Public Façade Per Library

Each library exports **one** umbrella header:

```cpp
// include/oran/<lib>.hpp
#pragma once
#include <oran/<lib>/<thing-1>.hpp>
#include <oran/<lib>/<thing-2>.hpp>
// ... only the symbols other libraries need
```

Consumers should write `#include <oran/agent.hpp>`, not
`#include <oran/agent/loop.hpp>`, in 90% of cases. The fine-grained headers exist for
when consumers care about TU cost.

## Forward-Declaration Shims

Some third-party libraries provide minimal forward-decl headers. Use them.

| Library         | Forward-decl header                        |
| --------------- | ------------------------------------------ |
| nlohmann_json   | `<nlohmann/json_fwd.hpp>`                  |
| spdlog          | wrap behind our own `oran-log` shim        |
| asio            | provide our own `<oran/async/awaitable_fwd.hpp>` |
| sqlite3         | wrap behind `<oran/storage/handle_fwd.hpp>` |
| cpp-httplib     | wrap behind `<oran/http/server_fwd.hpp>`   |
| libcurl         | hide entirely; only `oran-http::client` knows about it |

If a library does not ship a forward-decl header, **our own shim is mandatory**. It
contains only the names other libraries need to mention.

## Cross-Library Type Sharing

When two libraries need to share a type, it goes into `oran-core`. This is the *only*
sanctioned way to share types. Examples:

- `core::Message`, `core::Content`, `core::Role` — used by every layer above.
- `core::ToolDef`, `core::ToolUse`, `core::ToolResult` — agent + tool + provider all
  reference them.
- `core::Error`, `core::ErrorKind` — universal.

When a type starts to feel like it might belong in `oran-core` but is owned by a
specific subsystem, it goes into that subsystem's public header *and* gets a name that
makes the ownership clear (`memory::Record`, not `core::MemoryRecord`).

## What Goes In `oran-core`?

| In core                   | Not in core                                   |
| ------------------------- | --------------------------------------------- |
| `Result<T>`, `Error`      | `LogContext` (lives in `oran-log`)            |
| `Message`, `Content`      | `Conversation` (lives in `oran-storage`)      |
| `ToolDef`, `ToolUse`      | `Tool` runtime (lives in `oran-tool`)         |
| `Role`, `StopReason`      | `Provider` (lives in `oran-provider`)         |
| `Time`, `Duration`        | `Clock` services (lives in `oran-async`)      |
| `core::str::*`            | I/O helpers (live in `oran-io`)               |
| `Capability` enums        | concrete adapter classes                       |

If a type is borderline, default to "not core". Promotion to core requires a paragraph
of justification in the PR description.

## TU Cost Targets

| Category                      | Compile time per TU (Release, GCC 16.1, 1 core) |
| ----------------------------- | ----------------------------------------------- |
| Core / async / log / io       | ≤ 1.5 s                                         |
| Storage / config / permission | ≤ 2.0 s                                         |
| Tool / memory / hook / skill  | ≤ 2.5 s                                         |
| Provider / prompt / agent     | ≤ 3.0 s                                         |
| Orchestration / automation    | ≤ 3.0 s                                         |
| Channel adapters              | ≤ 3.0 s                                         |
| Web / CLI / bootstrap         | ≤ 4.0 s                                         |

`scripts/measure-tu.sh` (build skeleton) wraps `xmake -v` and emits per-TU times. CI
publishes them as artifacts and fails on regressions > 30% from baseline.

## Module-Aware Layout

When GCC 16.1's C++20 modules support is stable for our shape, the library structure
remains identical, but headers become module units:

```
src/<lib>/<lib>.cppm        // export module orangutan.<lib>;
src/<lib>/<lib>.internal.cppm  // module orangutan.<lib>:internal;
```

The public umbrella header stays as a *transitional* artifact so consumers can opt in.
See [`../rules/module-and-pch.md`](../rules/module-and-pch.md) for the migration story.

## Anti-Patterns To Reject In Review

- Any new `#include <nlohmann/json.hpp>` in `include/oran/`.
- Any public type whose private members include a non-pimpl asio/sqlite/curl/cpp-httplib
  type.
- Library X reaching into library Y's `src/Y/*.hpp` (only `include/oran/Y/*.hpp` is
  public).
- `friend class` across libraries.
- Templates whose definitions live in headers and *also* whose instantiations are
  expensive (consider extern template or moving to a `.cpp`).
- Macros that wrap control flow (`#define TRY(x) ...`) — concepts/`expected` instead.
