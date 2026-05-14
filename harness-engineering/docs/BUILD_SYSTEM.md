# Build System

Orangutan v2 uses **xmake** with **GCC 16.1** as the primary toolchain. This file
captures the build philosophy; mechanical compile-time discipline lives in
[`FAST_COMPILATION.md`](FAST_COMPILATION.md) and [`rules/compile-budget.md`](rules/compile-budget.md).

> Why xmake (kept): the legacy project already used xmake, the team is fluent in it,
> the lockfile workflow is good, and per-target packaging is clean.
>
> Why GCC 16.1: by the v2 timeframe it ships stable C++23 modules support, mature
> `std::expected`, `std::generator`, deducing-`this`, P2300 utilities, and the HALO
> improvements that matter for our coroutine surface. Clang ≥ 19 is supported as a
> secondary toolchain.

## Toolchain Requirements

```text
Primary:
  - GCC 16.1+
  - xmake 2.9+
  - libsodium development files (system or fetched by xmake)
  - libcurl development files (system) for transport
  - sqlite3 source (fetched by xmake; FTS5 enabled)

Secondary (CI matrix):
  - Clang 19+
  - musl-clang for static builds (stretch)
```

Disable any toolchain combination at configure time:

```sh
xmake f --toolchain=gcc       # default
xmake f --toolchain=clang
xmake f --runtime=c++_static  # static c++ runtime (stretch)
```

## Top-Level xmake Files

```
xmake.lua                # project-wide settings, includes the rest
xmake/options.lua        # CLI options (channels, sanitizers, modules, etc.)
xmake/toolchain.lua      # GCC 16.1 flags + module setup
xmake/packages.lua       # third-party requirements
xmake/targets.lua        # one entry per library + binary
xmake/tests.lua          # one entry per tests/<lib>/ bucket
xmake/bench.lua          # one entry per bench/<lib>/ bucket
xmake/checks.lua         # CI-friendly checks (deps, includes, TU times)
```

## Project-Wide Settings

```lua
-- xmake.lua
set_project("orangutan-v2")
set_version("2.0.0")
set_languages("c++23")           -- ratchet to c++26 only behind features
set_warnings("all", "extra")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".", lsp = "clangd"})
set_policy("package.requires_lock", true)
set_policy("build.optimization.lto", true)   -- release LTO
set_policy("build.warning", true)            -- warnings as errors via -Werror

includes("xmake/options.lua")
includes("xmake/toolchain.lua")
includes("xmake/packages.lua")
includes("xmake/targets.lua")
includes("xmake/tests.lua")
includes("xmake/bench.lua")
includes("xmake/checks.lua")
```

## Toolchain Configuration

```lua
-- xmake/toolchain.lua
toolchain("oran-gcc")
    set_homepage("GCC 16.1 with our flags")
    set_kind("standalone")
    set_toolset("cc",  "gcc-16")
    set_toolset("cxx", "g++-16")
    set_toolset("ld",  "g++-16")
    on_load(function (toolchain)
        toolchain:add("cxxflags",
            "-fdiagnostics-color=always",
            "-pipe",
            "-fdiagnostics-show-template-tree",
            "-fno-plt",
            "-fmacro-prefix-map=" .. os.projectdir() .. "=.",
            "-fno-common"
        )
        if has_config("modules") then
            toolchain:add("cxxflags", "-fmodules")
        end
        if has_config("lto") then
            toolchain:add("cxxflags", "-flto=auto")
            toolchain:add("ldflags", "-flto=auto")
        end
        if has_config("hardened") then
            toolchain:add("cxxflags",
                "-D_FORTIFY_SOURCE=3",
                "-fstack-protector-strong",
                "-fcf-protection",
                "-fstack-clash-protection"
            )
        end
    end)
toolchain_end()
```

## Options

```lua
-- xmake/options.lua
option("modules")
    set_default(true)
    set_description("Enable C++23 modules (GCC 16.1 required)")
option_end()

option("lto")
    set_default(true)
    set_description("Enable LTO in release mode")
option_end()

option("hardened")
    set_default(true)
    set_description("Enable hardening flags")
option_end()

option("sanitizers")
    set_default(false)
    set_description("Enable ASan/UBSan in debug builds")
option_end()

option("channel_qq")       set_default(true)  set_showmenu(true)  option_end()
option("channel_discord")  set_default(false) set_showmenu(true)  option_end()
option("channel_slack")    set_default(false) set_showmenu(true)  option_end()
option("channel_telegram") set_default(false) set_showmenu(true)  option_end()
option("channel_webhook")  set_default(true)  set_showmenu(true)  option_end()

option("hook_lua")         set_default(false) set_showmenu(true)  option_end()
option("hook_wasm")        set_default(false) set_showmenu(true)  option_end()

option("vector_memory")    set_default(false) set_showmenu(true)  option_end()
```

## Packages

```lua
-- xmake/packages.lua
add_requires("nlohmann_json   3.12.0", { configs = { header_only = true } })
add_requires("fmt             12.1.0", { configs = { header_only = true } })
add_requires("spdlog          1.17.0", { configs = { fmt_external = true } })
add_requireconfs("spdlog.fmt", { override = true, version = "12.1.0", configs = { header_only = true } })

add_requires("libsodium       1.0.20")        -- crypto_secretbox + Argon2id
add_requires("libcurl         8.11.0", { configs = { openssl = true } })
add_requires("sqlite3         3.52.0", { configs = { cflags = "-DSQLITE_ENABLE_FTS5" } })
add_requires("re2             2024.07.02")    -- runtime regex; replaces ctre
add_requires("asio            1.31.0", { configs = { ssl = "openssl" } })
add_requires("cli11           2.6.1")
add_requires("magic_enum      0.9.7")
add_requires("rapidhash       1.0")
add_requires("simdutf         8.0.0")
add_requires("replxx          2021.11.25")     -- REPL
add_requires("cpp-httplib     0.37.2")         -- only oran-web; keep encapsulated

add_requires("catch2          3.7.1", { configs = {} })
add_requires("nanobench       4.3.11")

if has_config("channel_qq")       then add_requires("libcurl") end
if has_config("hook_lua")         then add_requires("sol2 3.5.0", { configs = { lua = "luajit" } }) end
if has_config("vector_memory")    then add_requires("sqlite-vec 0.1.7-alpha.2") end
```

**Notable removals vs. legacy:**

- ❌ `stdexec-gtc` (custom NVIDIA fork) — replaced by `asio` + coroutines.
- ❌ `mbedtls` — replaced by `libsodium` for secret crypto; TLS goes through OpenSSL via libcurl.
- ❌ `ctre` — replaced by `re2` (runtime patterns; smaller TU).
- ❌ `uni_algo` — folded into `oran-core::str` using stdlib + simdutf for the bits we need.

## Library Targets

Each library follows the same shape:

```lua
-- xmake/targets.lua
local root = os.projectdir()

local function oran_lib(name, deps, packages)
    target("oran-" .. name)
        set_kind("static")
        set_group("oran-libs")
        add_includedirs(path.join(root, "include"), { public = true })
        add_files(path.join(root, "src", "oran-" .. name, "**.cpp"))
        if deps    then add_deps(table.unpack(deps)) end
        if packages then add_packages(table.unpack(packages), { public = false }) end
        add_packages("fmt", { public = true })    -- in PCH; cheap
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
end

oran_lib("core",          {},                    { "magic_enum", "rapidhash", "simdutf", "nlohmann_json" })
oran_lib("async",         {"oran-core"},         { "asio" })
oran_lib("log",           {"oran-core"},         { "spdlog", "fmt", "re2" })
oran_lib("io",            {"oran-core","oran-async"}, {})
oran_lib("http",          {"oran-core","oran-async"}, { "libcurl", "asio" })
oran_lib("storage",       {"oran-core"},         { "sqlite3" })
oran_lib("config",        {"oran-core","oran-storage"}, { "nlohmann_json", "libsodium" })
oran_lib("permission",    {"oran-core","oran-log"}, { "re2" })
oran_lib("skill",         {"oran-core","oran-io"}, {})
oran_lib("hook",          {"oran-core","oran-async","oran-io"}, { "nlohmann_json" })
oran_lib("tool",          {"oran-core","oran-async","oran-permission","oran-hook","oran-io"}, { "nlohmann_json" })
oran_lib("memory",        {"oran-core","oran-storage","oran-async"}, { "nlohmann_json" })
oran_lib("provider",      {"oran-core","oran-async","oran-http"}, { "nlohmann_json" })
oran_lib("prompt",        {"oran-core","oran-memory","oran-skill"}, { "nlohmann_json" })
oran_lib("agent",         {"oran-core","oran-async","oran-provider","oran-tool",
                            "oran-memory","oran-prompt","oran-permission","oran-hook"}, {})
oran_lib("orchestration", {"oran-agent","oran-async","oran-storage"}, {})
oran_lib("automation",    {"oran-agent","oran-storage","oran-async"}, {})
oran_lib("channel",       {"oran-async","oran-http","oran-agent"}, {})

if has_config("channel_qq")       then oran_lib("channel-qq",       {"oran-channel","oran-http"}, {}) end
if has_config("channel_discord")  then oran_lib("channel-discord",  {"oran-channel","oran-http"}, {}) end
if has_config("channel_slack")    then oran_lib("channel-slack",    {"oran-channel","oran-http"}, {}) end
if has_config("channel_telegram") then oran_lib("channel-telegram", {"oran-channel","oran-http"}, {}) end
if has_config("channel_webhook")  then oran_lib("channel-webhook",  {"oran-channel","oran-http"}, {}) end

oran_lib("web",  {"oran-agent","oran-orchestration","oran-http"}, { "cpp-httplib" })
oran_lib("cli",  {"oran-agent","oran-orchestration"}, { "replxx" })
oran_lib("bootstrap",
    {"oran-cli","oran-web","oran-channel","oran-orchestration","oran-automation","oran-config"},
    { "cli11" })

target("orangutan")
    set_kind("binary")
    add_deps("oran-bootstrap")
    add_files(path.join(root, "src/main.cpp"))

target("orangutan-server")
    set_kind("binary")
    add_deps("oran-web","oran-channel","oran-automation","oran-bootstrap")
    add_files(path.join(root, "src/server.cpp"))

target("orangutan-bench")
    set_kind("binary")
    add_deps("oran-async","oran-core")
    add_files(path.join(root, "src/bench-main.cpp"))
```

**Key compile-time wins from this shape:**

- Each library is its own static archive; touching `oran-channel-qq` does not recompile
  `oran-tool`.
- The PCH (`include/oran/_pch.hpp`) is shared across all libraries; cost is paid once.
- Public headers (`include/oran/<lib>/`) are stable; impl-private headers in
  `src/oran-<lib>/` change freely without ripples.

## PCH

`include/oran/_pch.hpp` includes only stable, low-cost headers:

```cpp
#pragma once

// Stdlib stable headers
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Header-only forward-decls
#include <fmt/core.h>
#include <nlohmann/json_fwd.hpp>

// Project-wide foundations (intentionally small)
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
```

Things deliberately **excluded** from the PCH:

- `<asio.hpp>` — too heavy; `oran-async` adds it locally.
- `<spdlog/spdlog.h>` — too heavy; `oran-log` adds it locally.
- `<httplib.h>` — only `oran-web` needs it.
- `<sqlite3.h>` — only `oran-storage` needs it.
- Anything from `<ranges>`, `<format>`, `<regex>` — too heavy or unstable.

Per-target PCH override is allowed only if measurement proves > 1s saving (record in
`docs/exec-plans/tech-debt-tracker.md`).

## Modules

GCC 16.1 supports C++20 modules. We adopt them **incrementally**:

1. Phase 1 (MVP): traditional headers + PCH. Modules opt-in via `--modules=y` for
   experiments.
2. Phase 2: `oran-core` and `oran-async` migrate to module units (`*.cppm`).
3. Phase 3: every library migrates.

See [`rules/module-and-pch.md`](rules/module-and-pch.md) for the migration recipe and
fallback when modules misbehave.

## Build Commands

```sh
# Configure
xmake f -m release                 # release
xmake f -m debug --sanitizers=y    # debug + ASan/UBSan
xmake f -m release --modules=y     # release + modules

# Build the whole project
xmake -j$(nproc)

# Build only one library
xmake build oran-tool

# Run tests for one library
xmake build test-tool && xmake run test-tool

# Run a benchmark
xmake build bench-memory && xmake run bench-memory

# Cross-implementation bench compare
scripts/bench-compare.sh memory
```

## Compile-Time Targets

A clean release build on an 8-core/16 GB machine targets:

| Stage             | Target time | Hard cap |
| ----------------- | ----------- | -------- |
| Configure         | ≤ 5 s       | 20 s     |
| Build all libs    | ≤ 25 s      | 45 s     |
| Link binaries     | ≤ 5 s       | 15 s     |
| Incremental rebuild after 1 .cpp edit | ≤ 3 s | 10 s |

Hard cap exceedance fails CI. See [`rules/compile-budget.md`](rules/compile-budget.md).

## Sanitizers

In debug + `--sanitizers=y`, ASan + UBSan are enabled. TSan is opt-in via
`--sanitizers=tsan` because it conflicts with ASan and adds significant overhead.

## Release Stripping

Release binaries are stripped by xmake's default (`set_strip("all")` per target).
A separate `.debug` artifact is produced for distribution; CI uploads it.

## Static Linking

Optional: `--static=y` produces a fully-static binary via musl-clang (stretch). Useful
for distributing the bench runner.

## Reproducibility

- `set_policy("package.requires_lock", true)` pins package versions via
  `xmake-requires.lock`.
- `-fmacro-prefix-map` strips the build dir from `__FILE__` strings.
- `SOURCE_DATE_EPOCH` honored if set (CI sets it from the commit timestamp).

## Anti-Patterns

- Adding a `.cpp` to a library without checking it compiles in budget.
- Adding a heavy include to a public header.
- Pulling a new third-party package without an entry in
  [`rules/libraries.md`](rules/libraries.md).
- Disabling LTO "for now" without an issue tracking when it's re-enabled.

## See Also

- [`FAST_COMPILATION.md`](FAST_COMPILATION.md) — the engineering playbook.
- [`rules/compile-budget.md`](rules/compile-budget.md) — the enforced budget.
- [`rules/module-and-pch.md`](rules/module-and-pch.md) — PCH/module mechanics.
- [`rules/libraries.md`](rules/libraries.md) — library approval list.
