# Fast Compilation — The Playbook

This file is the **engineering playbook** for keeping compile times honest. The legacy
`orangutan/` reached ~70 s clean builds and minutes of incremental on 16 GB RAM. We
will not repeat that mistake.

> The single biggest predictor of compile time in a C++ project is *what gets included
> in each translation unit*. Every other optimization is a multiplier on that base
> cost. Treat include hygiene as the highest-leverage activity.

## Why It Matters

| Compile time per change | Iteration loop                                  | Outcome              |
| ----------------------- | ----------------------------------------------- | -------------------- |
| < 5 s                   | Edit-run cycle is fluid                          | Healthy project       |
| 5 – 15 s                | Tolerable for back-to-back edits                 | Slipping              |
| 15 – 60 s               | Devs batch changes and stop running tests often  | Sliding into legacy   |
| > 60 s                  | Devs avoid the build entirely                    | What we're escaping   |

We aim for the **< 5 s** band on incremental builds for the working library.

## Twelve Levers (Ordered By Impact)

### 1. Forward-Declare In Public Headers

**Highest-impact single rule.** A public header may include only:

- Standard library headers from the PCH set (see `BUILD_SYSTEM.md`).
- Other `oran-core` public headers (intentionally small).
- Forward-decl shims (`<nlohmann/json_fwd.hpp>`, `<oran/async/awaitable_fwd.hpp>`, …).

Anything else moves to the `.cpp` (or a `src/<lib>/_impl/<lib>.hpp` private header).

**Enforcement:** `scripts/check-includes.sh` walks `include/oran/` and rejects any
file whose top-level `#include`s exceed the PCH set unless explicitly whitelisted.

### 2. Pimpl Heavy Classes

Any public class whose private members would force public includes of heavy types
uses the pimpl idiom (`std::unique_ptr<Impl>`). The `Impl` lives in a `_impl/` private
header; the public class's source file is the only TU that sees the heavy types.

**Pimpl candidates by definition:**

- Owns an asio object.
- Owns a JSON object.
- Owns a SQLite/libcurl/spdlog handle.
- Has > 8 fields.

### 3. PCH For Stable Headers Only

`include/oran/_pch.hpp` (see `BUILD_SYSTEM.md`). Strictly stable headers; nothing
that mutates during normal development. The PCH is shared across all targets.

**Anti-pattern:** putting `<asio.hpp>` in the PCH — touches the PCH any time an asio
header bumps, which invalidates every TU.

### 4. C++20/23 Modules Where Stable

GCC 16.1 supports modules. When the surface is stable, prefer:

```cpp
// src/oran-core/core.cppm
export module orangutan.core;
export import :error;
export import :result;
export import :time;
```

over header-only. Modules avoid re-parsing the same code in every TU.

**Caveat:** the toolchain has rough edges. Don't push modules into experimental
shapes. See `rules/module-and-pch.md` for the migration recipe.

### 5. Unity Builds For Cold Modules

Libraries that change rarely (channel adapters, automation seed data) can be unity-built:

```lua
target("oran-channel-qq")
    set_kind("static")
    add_rules("c++.unity_build", { batchsize = 0 })
    add_files("src/oran-channel-qq/**.cpp")
```

A unity build merges all `.cpp` files into one TU, eliminating duplicate header parses.

**Caveat:** invalidates incremental builds — *every* edit recompiles the whole TU.
Use only for libraries you rarely touch.

### 6. Limit Template Instantiations Per TU

- Don't put template definitions in public headers unless the template is small
  (< 30 lines) or used in 1–2 places.
- Use `extern template` declarations for templates instantiated in many TUs.
- Prefer concept-bounded function templates over class templates for "callback shapes".

### 7. Replace Heavy Header-Only Libraries

The legacy project's biggest header-only weights were:

| Lib               | Alternative or mitigation                                  |
| ----------------- | --------------------------------------------------------- |
| `stdexec`         | asio + coroutines (smaller surface, smaller TU)            |
| `ctre`            | re2 (runtime, smaller TU)                                  |
| `nlohmann_json` in headers | forward-decl + `.cpp` includes                     |
| `cpp-httplib`     | hide entirely behind `oran-web` and `oran-http::Client`    |
| `spdlog/spdlog.h` | thin `oran-log` shim with macro-style API                  |

### 8. Disable RTTI / Exceptions Where Possible

Project default: RTTI **enabled**, exceptions **enabled** (asio relies on both). But:

- Per-target overrides are allowed where measurement justifies it.
- The `oran-bench` binary's tight inner loops can build with `-fno-exceptions` on
  cherry-picked TUs (record in `tech-debt-tracker.md`).

### 9. Reduce Constexpr / Consteval Surface

GCC 16.1's compile-time evaluator is fast but not free. Avoid using `consteval`
helpers in hot headers. If `magic_enum` becomes expensive (it has a per-enum compile
cost), wrap it behind a `.cpp`-defined `enum_name()` for hot enums.

### 10. Measure Before Optimizing

`scripts/measure-tu.sh` (build skeleton) emits per-TU compile times. Sort, look at
the top 20, then act:

```sh
scripts/measure-tu.sh --release --json | jq -r 'sort_by(.seconds) | reverse | .[:20]'
```

### 11. CI Tracks Compile Time As A Test

`scripts/check-compile-budget.sh` (build skeleton):

- Runs a clean build with `-ftime-report`.
- Parses per-TU times.
- Compares against `compile_budget.json` (committed).
- Fails the build on > 30% regression.

### 12. Don't Bundle Things

Each library is its own xmake target. `oran-tool` does **not** include
`oran-orchestration` headers, even though their concepts are related. If you find
yourself reaching for another library's header inside your library's source, ask
whether the dependency belongs in the architecture map.

## Worked Examples

### Example A — A Library That Imports Too Much

Before:

```cpp
// include/oran/agent/loop.hpp
#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "provider.hpp"   // pulls in libcurl indirectly
#include "tool.hpp"
#include "memory.hpp"
namespace orangutan::agent {
class Loop {
  asio::cancellation_signal cancel_;
  nlohmann::json scratch_;
  std::shared_ptr<provider::System> system_;
  // ...
};
}
```

TUs that `#include <oran/agent/loop.hpp>` pay 1.5 s+ for asio alone.

After:

```cpp
// include/oran/agent/loop.hpp
#include <memory>
#include <string>
#include <oran/core/result.hpp>
#include <oran/async/awaitable_fwd.hpp>
namespace orangutan::provider { class System; }
namespace orangutan::tool     { class Registry; }
namespace orangutan::memory   { class Runtime; }
namespace orangutan::agent {
class Loop {
 public:
  Loop(provider::System&, tool::Registry&, memory::Runtime&);
  ~Loop();
  async::Awaitable<core::Result<RunResult>> run(std::string prompt);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
```

TUs now pay ~50 ms for the header. The `.cpp` pays the full cost once.

### Example B — Template That Got Out Of Hand

Before — recursive `if constexpr` ladder in a public header:

```cpp
// include/oran/core/visit.hpp
template <typename V, typename F>
constexpr auto visit_typed(V&& v, F&& f) {
  if constexpr (std::is_same_v<std::decay_t<V>, A>) ...
  else if constexpr (std::is_same_v<std::decay_t<V>, B>) ...
  // ten more branches
}
```

Every TU that includes it instantiates the ladder.

After — use `std::visit` + a tiny `Overloaded` helper (already in legacy `utils`):

```cpp
// include/oran/core/overloaded.hpp
template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;
```

Variant visitors instantiated where actually used; no public template ladder.

### Example C — Coroutine Frame Bloat

Coroutines on the agent's hot path were heap-allocating because the compiler couldn't
elide them. Two changes:

1. Mark inner coroutines `[[nodiscard]]` and inline-only.
2. Avoid `std::function` along the call chain (use concept-bounded templates).

Measure with `-fcoroutines -frounding-math -ftime-report` and check the coroutine
section in `-ftime-report` output.

## Process Discipline

### Every PR

- `make ci` runs the doc + hygiene check.
- `xmake build` runs as usual.
- `scripts/check-compile-budget.sh` runs in CI. Regressions > 30% fail the build.

### Quarterly

- Walk the top-20 TUs in `scripts/measure-tu.sh --json`.
- Open exec plans for the worst offenders.
- Run `scripts/clean-build-time.sh` to record a fresh baseline; commit it as
  `docs/generated/compile-baseline-YYYY-MM-DD.json`.

### Whenever A TU Grows > 5s

- Open an issue with the file, the time, and the headers responsible.
- Land a forward-decl / pimpl / module migration; record the result in
  `docs/exec-plans/tech-debt-tracker.md`.

## A Quick Reality Check

C++ compile time is partly architecture, partly toolchain. We assume:

- Single-core compile times mostly capture *architecture* problems.
- Parallel compile times capture *toolchain* problems (link contention, RAM pressure).

When metrics diverge between single-core and parallel runs, suspect parallelization
issues (link bottleneck, RAM exhaustion) and bisect accordingly.

## See Also

- [`BUILD_SYSTEM.md`](BUILD_SYSTEM.md) — toolchain, packages, target layout.
- [`rules/compile-budget.md`](rules/compile-budget.md) — the enforced numbers.
- [`rules/module-and-pch.md`](rules/module-and-pch.md) — module migration recipe.
- [`design-docs/module-boundaries.md`](design-docs/module-boundaries.md) — why the
  library shape exists.
