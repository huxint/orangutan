# `include/` — Public Headers

This tree holds **public** C++ headers for the project. Conventions:

- One subdirectory per library: `include/oran/<lib>/*.hpp`.
- One umbrella header per library: `include/oran/<lib>.hpp` (re-exports submodule
  public symbols).
- Project-wide PCH: `include/oran/_pch.hpp` (stdlib-stable headers + `core::Result`
  + `core::Error`).
- Forward-declaration shims: `include/oran/async/awaitable_fwd.hpp`,
  `include/oran/storage/handle_fwd.hpp`, etc.

## Rules

Read [`docs/rules/critical-rules.md#C6`](../docs/rules/critical-rules.md), the
[`docs/design-docs/module-boundaries.md`](../docs/design-docs/module-boundaries.md)
file, and [`docs/FAST_COMPILATION.md`](../docs/FAST_COMPILATION.md) before adding a
new header.

In short:

- No heavy includes (`nlohmann/json.hpp`, `asio.hpp`, `spdlog/spdlog.h`, `httplib.h`,
  `sqlite3.h`, `curl/curl.h`, `re2/re2.h`).
- Pimpl wherever you'd otherwise force a heavy include.
- One public class per header.
- Header guard via `#pragma once`.

## Status

Empty. The first library to land here is `oran-core` per
[`docs/product-specs/0001-core-react-loop.md`](../docs/product-specs/0001-core-react-loop.md).
