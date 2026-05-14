# `src/` — Library Implementations

One subdirectory per library: `src/oran-<lib>/`. Each library:

- Compiles as a `static` xmake target named `oran-<lib>`.
- Has its public headers under [`../include/oran/<lib>/`](../include).
- Owns private headers under `src/oran-<lib>/_impl/`.
- Has a `tests/<lib>/` and `bench/<lib>/` neighbour.

## Conventions

- Implementations may include heavy third-party headers (`nlohmann/json.hpp`,
  `asio.hpp`, `sqlite3.h`, `spdlog/spdlog.h`).
- File naming: `kebab-case.cpp` matching the primary class.
- One primary class per file; helpers in anonymous namespace or `_impl/`.
- For module-aware libraries (phase 1+), the module unit is `<lib>.cppm`.

## Adding A New Library

1. Open an execution plan: `make new-plan SLUG=add-<lib>`.
2. Add the library to [`../xmake/targets.lua`](../xmake) with `add_deps()` matching
   the allowed dependencies in
   [`../docs/ARCHITECTURE.md#library-inventory`](../docs/ARCHITECTURE.md).
3. Add `tests/<lib>/` and `bench/<lib>/` with at least a `placeholder.cpp`.
4. Document the library in `../docs/design-docs/<lib>.md` if its API surface is
   substantial.
5. Update [`../docs/rules/libraries.md`](../docs/rules/libraries.md) if you pull in
   new third-party deps.
6. Write a history entry.

## Status

Empty. See [`../docs/product-specs/index.md`](../docs/product-specs/index.md) for
the planned libraries.
