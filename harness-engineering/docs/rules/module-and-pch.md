# Modules And PCH

This file defines:

- The PCH set.
- The C++20/23 module migration recipe.
- Fallback rules when modules misbehave.

> Background: GCC 16.1 supports C++20 modules well for the shape we care about
> (library-internal `module orangutan.<lib>` + `import :submodule` partitions). The
> stdlib (`import std;`) is supported but we defer adoption until we measure stability
> in CI.

## PCH

### The Set

`include/oran/_pch.hpp` (project-wide; identical contents across all targets):

```cpp
#pragma once

// Stdlib stable
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Header-only / forward-decl-only
#include <fmt/core.h>
#include <nlohmann/json_fwd.hpp>

// Project foundations (deliberately small)
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
```

### Rules

- The PCH **never** includes headers that frequently change (asio, spdlog full, sqlite,
  curl, httplib, json full, ctre, stdexec).
- A new addition requires:
  - Justification that the header is genuinely stable (rarely bumped).
  - Measurement showing aggregate compile-time benefit > 0.5 s across the project.
  - Approval recorded in the PR.
- Per-target PCH override is allowed only behind a measurement-backed exec plan and
  records the saving in `docs/exec-plans/tech-debt-tracker.md`.

### Enforcement

`scripts/check-pch.sh` compares the actual PCH contents with the approved list
(`docs/generated/pch-spec.json`). Drift requires a PR to the spec.

## Modules

### Phase Plan

| Phase | Scope | Status target |
| --- | --- | --- |
| 0 | Header + PCH only. | v1 release. |
| 1 | `oran-core` and `oran-async` ship as module units (`*.cppm`). Headers remain as a transitional layer for consumers not yet on modules. | v1.1. |
| 2 | All `oran-*` libraries migrate. Headers become wrappers that re-export the module's public symbols. | v1.2. |
| 3 | `import std;` adopted if measurement shows ≥ 10% project-wide build speedup. | v2.0. |

### File Layout

Per library:

```
src/oran-<lib>/<lib>.cppm                 // export module orangutan.<lib>;
src/oran-<lib>/<lib>.public.cppm          // module partitions for public symbols
src/oran-<lib>/<lib>.internal.cppm        // module partitions for internal symbols
src/oran-<lib>/*.cpp                      // module-internal implementations
```

A library's umbrella header (`include/oran/<lib>.hpp`) becomes:

```cpp
// include/oran/<lib>.hpp — transitional during phases 1–2
#pragma once
#ifdef ORAN_USE_MODULES
import orangutan.<lib>;
#else
#include <oran/<lib>/<thing-1>.hpp>
#include <oran/<lib>/<thing-2>.hpp>
#endif
```

### Recipe — Migrating One Library

1. **Identify** the library's public symbols (everything listed in the umbrella header).
2. **Create** `src/oran-<lib>/<lib>.cppm`:

   ```cpp
   module;
   #include <cstddef>
   #include <string>
   export module orangutan.<lib>;
   export import :public_api;
   import :internal_api;
   ```

3. **Split** the existing implementations into module partitions:

   ```cpp
   // <lib>.public.cppm
   module orangutan.<lib>:public_api;
   export namespace orangutan::<lib> { class Foo { ... }; }
   ```

4. **Update** `xmake/targets.lua` for the library to compile module units. xmake
   detects `.cppm` files automatically when modules are enabled.
5. **Keep** the umbrella header in place as a transitional wrapper.
6. **Measure** before/after with `scripts/measure-tu.sh`. Record the result in the
   migration's exec plan.

### Toolchain Quirks To Plan For

GCC 16.1 modules have known sharp edges. Plan for:

- **Module unit ordering** must respect dependency order during single-target builds.
  xmake handles this if `add_files(.../**.cppm)` is correctly grouped.
- **BMI cache** under `build/.modules/` — clean with `xmake clean modules` if mismatches
  appear.
- **Compiler error messages** point at preprocessed module units; a small awareness in
  troubleshooting docs.
- **clangd modules support** is improving but uneven. Until it stabilizes, we keep the
  umbrella header for editor-time include hints.

### Module / Header Equivalence Rules

While both shapes exist:

- The umbrella header **must** re-export the same symbols as the module.
- The CI matrix runs the library both ways for at least one TU to catch divergence.
- If a name is added to the module but not the header (or vice versa), `scripts/
  check-module-parity.sh` fails.

## Fallback / Disablement

`xmake f --modules=n` builds the project header-only. This shape:

- Must always work.
- Is the canary for "are we sure modules are pure optimization?"
- Is what the legacy project effectively had.

If GCC ships a regression that breaks modules in our shape, we fall back via this
flag and open an exec plan to track upstream resolution.

## Anti-Patterns

- Half-migrated libraries where the module declares more than the header. Migrate
  fully or not at all.
- Module units that include heavy headers in their global-module fragment. The point
  of modules is that *consumers* don't see those headers; the module unit itself still
  has to compile them once.
- Macros that change behavior across module/header boundaries. Macros don't cross
  module boundaries; rely on a constant `inline constexpr` in `oran-core` instead.

## See Also

- [`compile-budget.md`](compile-budget.md) — what the migration is in service of.
- [`../FAST_COMPILATION.md`](../FAST_COMPILATION.md) — the broader playbook.
- [`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md) — xmake module wiring.
