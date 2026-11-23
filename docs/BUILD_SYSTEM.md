# Build System

Supported toolchain: **Linux**, **GCC 16.1+** (C++26 reflection), **xmake 3.1.1+**, system `libcurl` dev headers and `pkg-config`.
Desktop UI (`apps/desktop`) uses **Node.js 24+** and npm independently of xmake.

## Common Commands

```sh
# Configure, build default runtime libs + `orangutan` executable, and run all 15 test targets
xmake f -y -m release
xmake build -j4
xmake test -j4

# Iterate on one test bucket
xmake build test-tool && xmake run test-tool

# Debug + ASan/UBSan
xmake f -y -m debug --sanitizers=y
xmake build -j4 && xmake test -j4

# Opt-in native Vulkan desktop, channel hosts & evaluations
xmake build -j4 orangutan-desktop # requires system SDL3, FreeType2, Fontconfig, libpng16, libvulkan
xmake build -j4 oran-telegram
xmake build -j4 oran-qq-login     # requires system libqrencode 4.1.1
xmake build -j4 eval-context eval-qq

# Repository contract checks (independent of C++ build)
make ci

# Desktop workbench build, unit tests & format check
npm --prefix apps/desktop ci
npm --prefix apps/desktop run build
npm --prefix apps/desktop test
npm --prefix apps/desktop run format:check
```

## Build Layout

| File | Owns |
| --- | --- |
| `xmake.lua` | C++26 standard, warnings, `oran-gcc` toolchain and `oran.build` policy. |
| `xmake/targets.lua` | 15 static `oran-*` library targets. |
| `xmake/apps.lua` | Default `orangutan` Personal Agent binary plus opt-in `oran-telegram` / `oran-qq-login`. |
| `xmake/packages.lua` | Pinned third-party dependencies (synced with [libraries.md](rules/libraries.md)). |
| `xmake/tests.lua`, `xmake/bench.lua`, `xmake/evals.lua` | Catch2 tests, nanobench targets and opt-in evaluations. |

## Build Options & Compile Hygiene

| Option | Default | Effect |
| --- | --- | --- |
| `lto` | On | Link-time optimization in release builds. |
| `sanitizers` | Off | ASan/UBSan in debug builds. |
| `hardened` | Off | `_FORTIFY_SOURCE=3`, stack and control-flow protection. |
| `analyze` | Off | GCC `-fanalyzer` with analyzer warnings promoted to errors. |

Public headers (`include/oran/`) keep heavy third-party definitions (`nlohmann/json`, `asio`, `sqlite3`, `libcurl`, `re2`) private to `.cpp` files and share `include/oran/_pch.hpp`, enforced by `scripts/check-includes.sh`.
