# Xmake Build, Dependency, And Test Migration Design

**Date:** 2026-03-23
**Status:** Approved

## Overview

Migrate `orangutan` from CMake to `xmake` as the only supported build system, move all third-party dependencies under `xmake/xrepo` management, replace `xxhash` with `rapidhash`, replace `gtest` with `boost-ext::ut`, replace `readline` with `replxx`, and replace `OpenSSL` with `Mbed TLS`.

This is a single delivery with no permanent transition period. The repository should end in a state where `xmake` is the canonical build and test entry point, CMake is removed, and the old dependency stack no longer exists in source, scripts, or build configuration.

The migration also fixes a current testing weakness: many tests share process-global state and deterministic temp paths, so they fail under parallel execution even though disabling parallelism makes the suite unacceptably slow. The new test system must be modular, process-parallel, and isolated by default.

## Goals

- Remove `CMakeLists.txt` and make `xmake` the only supported build system.
- Manage all third-party libraries through `xmake/xrepo`.
- Support Linux with both `gcc` and `clang`.
- Replace `xxhash` with `rapidhash`.
- Replace `gtest` with `boost-ext::ut`.
- Replace `readline` with `replxx`.
- Replace `OpenSSL` with `Mbed TLS`.
- Keep the intended current functionality moving forward, including CLI, web, storage, config secret protection, and QQ channel support.
- Redesign the test layout so module-level test binaries can run in parallel safely and faster than the current suite.

## Non-Goals

- Do not do a broad source-tree reorganization unrelated to the migration.
- Do not preserve old formats or behaviors purely for backward compatibility if a cleaner migration result is better for the in-progress codebase.
- Do not widen platform support beyond Linux in this pass.
- Do not keep a dual-build setup with both CMake and `xmake`.
- Do not solve unrelated refactors that do not help the migration, dependency replacement, or test isolation work.

## Constraints And Assumptions

- Linux is the only target that must be made stable now.
- `gcc` and `clang` are both first-class toolchains.
- All third-party dependencies must be managed by `xmake`, including libraries previously discovered from the system.
- If an upstream `xrepo` package or option does not exactly fit project needs, the repository may add local `xmake` package recipes or overlays. This still satisfies the requirement that third-party management flows through `xmake`.
- The final state must not depend on CMake, `pkg-config`, `find_package`, or system-installed package discovery.

## Current State

The current build is centralized in `CMakeLists.txt` and mixes:

- `find_package(...)` for `nlohmann_json`, `spdlog`, `CLI11`, `CURL`, `Threads`, and `OpenSSL`
- `pkg_check_modules(...)` for `readline` and `sqlite3`
- `FetchContent` for `tomlplusplus`, `xxhash`, `cpp-httplib`, `stdexec`, Boost headers for QQ support, and `googletest`

Tests are built as a single `orangutan_tests` binary with `gtest_discover_tests(...)`. This makes it easy to write tests but causes two structural problems:

- many tests share process-level state such as environment variables and default logger configuration
- many tests use fixed temp directories, fixed DB file names, or deterministic workspace roots

This layout makes cross-module parallel execution fragile and encourages hidden coupling.

Two implementation details drive the dependency plan:

- `src/features/tools/core/hashline.cpp` directly includes `xxhash.h`
- `src/features/channel/qq/transport.cpp` uses `Boost.Asio/Beast` over `OpenSSL` for QQ WebSocket TLS

The second point means `OpenSSL -> Mbed TLS` is not just a crypto-library rename for config secrets. The QQ transport must also move off the OpenSSL-based stack.

## Approaches Considered

### Option A: Mechanical dependency replacement with minimal restructuring

- Convert the build to `xmake`
- Replace libraries in place
- Keep the current single test binary shape

Pros:

- Lowest initial churn.

Cons:

- Does not fix the test parallelism problem.
- Leaves process-global test coupling in place.
- Makes the QQ `OpenSSL -> Mbed TLS` change harder because the transport still assumes the old TLS stack.

### Option B: Large source-tree refactor plus build migration

- Redesign production modules and directory layout first
- Then migrate build and dependencies

Pros:

- Could produce a cleaner long-term architecture.

Cons:

- Too much movement for one delivery.
- High regression risk.
- Harder to validate because structural and dependency changes are mixed together.

### Option C: Targeted internal adapters plus one-shot migration

- Keep the current source layout mostly intact
- Introduce a few narrow internal abstraction points where dependency replacements need them
- Convert build, dependencies, and tests in one final cutover

Pros:

- Solves the actual migration problems without a broad rewrite.
- Supports a true one-shot delivery.
- Makes test isolation explicit.
- Contains the `OpenSSL -> Mbed TLS` impact to well-defined boundaries.

Cons:

- Requires careful planning of target boundaries and test helpers up front.

**Recommendation:** Option C.

## Key Decisions

### `xmake` becomes the only build entry point

The repository will have a root `xmake.lua` and supporting `xmake/*.lua` files for options, packages, targets, and tests. CMake files are removed in the final state.

`xmake` must support:

- debug and release modes
- `gcc` and `clang`
- generation of `compile_commands.json`
- a single default command path for building and testing

### All third-party dependencies flow through `xmake/xrepo`

Dependency management is centralized in `xmake`, including packages that are currently taken from the system.

Expected package groups:

- runtime/core: `nlohmann_json`, `spdlog`, `CLI11`, `curl`, `sqlite3`, `toml++`, `cpp-httplib`, `stdexec`
- replacements: `rapidhash`, `replxx`, `mbedtls`
- test: `boost-ext::ut`

If exact upstream package names or backend configuration are unavailable, the repository will add local package recipes and keep them under `xmake` control rather than falling back to system package discovery.

### Preserve current source layout, redefine build targets

The directory structure under `src/` remains largely intact for this migration. Instead of moving files aggressively, `xmake` defines cleaner link boundaries:

- `orangutan_runtime`
  - owns `src/core/*`, `src/infra/*`, `src/features/*`, `src/app/runtime/*`
- `orangutan_cli`
  - owns non-`main` app flow code such as `bootstrap`, `repl`, `single-shot`, `channel-serve`, `slash-commands`, and CLI UI
- `orangutan`
  - owns only `src/main.cpp`

This reduces migration risk while still making dependencies and tests more modular.

### `hashline` can change format if it improves the new implementation

The migration does not need to preserve old hash values, anchor strings, or serialized hashline output for backward compatibility. The only requirement is that the resulting edit and anchor model is deterministic, internally coherent, and well-covered by tests.

The implementation should still isolate the hash primitive behind a narrow helper rather than including `rapidhash` directly across the codebase. This keeps the new design reviewable and avoids re-coupling the feature to a specific hash header layout.

### REPL input logic gets a thin adapter

`readline` use is currently embedded directly in `src/app/repl.cpp`. The migration adds a narrow line-editor adapter and switches the implementation to `replxx`.

Behavior that should remain stable:

- prompt-driven single-line input
- trailing `\` continuation
- explicit multiline mode
- command history append
- EOF handling

This is a dependency swap, not a CLI UX redesign.

### Config secret protection can change format

The crypto implementation changes from `OpenSSL` to `Mbed TLS`, and it does not need to preserve the old protected config encoding for backward compatibility. The migration may adopt a cleaner or simpler payload format if it improves implementation quality.

The only requirements are:

- secrets can be protected and recovered reliably within the new codebase
- the format is explicit and test-covered
- the implementation no longer depends on `OpenSSL`

### QQ transport moves off the `OpenSSL` stack

The current QQ WebSocket transport depends on `Boost.Asio/Beast` plus `OpenSSL`. To fully remove `OpenSSL`, the QQ transport must move to a TLS path that can run on the same `libcurl + Mbed TLS` network stack used elsewhere.

Recommended direction:

- keep the public `qq::Transport` callback interface
- replace the current transport internals with a `libcurl`-based WebSocket implementation
- use the same TLS stack family as the HTTP client path

Behavior that must remain stable:

- connect / stop / reconnect API shape
- background-threaded transport model
- `on_open`, `on_text`, `on_close`, `on_error` callback semantics
- reconnect backoff behavior

This avoids keeping Boost networking and OpenSSL only for QQ support.

## Build And Package Design

### Root build layout

Use:

- `xmake.lua`
- `xmake/options.lua`
- `xmake/packages.lua`
- `xmake/targets.lua`
- `xmake/tests.lua`

The root file should stay thin and delegate to those modules.

### Toolchain support

The build must support:

- `xmake f --toolchain=gcc`
- `xmake f --toolchain=clang`

The project should set a shared language baseline equivalent to the current codebase requirements and avoid compiler-specific hard-coding like the current forced `clang++` setting in CMake.

### Package locking

The repository should check in the `xmake` lock file so dependency versions are reproducible. Version pinning is required for major third-party replacements and for transitive behavior that affects TLS or testing.

### Local package overlays when required

If needed, add repository-local package recipes for:

- packages not present in upstream `xrepo`
- packages that need exact feature or backend configuration
- `libcurl` backend constraints needed to keep the project on `Mbed TLS`

These overlays are preferred over ad hoc shell scripts or system package assumptions.

## Test Architecture

### Replace one large test binary with many module-focused binaries

The test suite should be split into multiple targets, for example:

- `test_core`
- `test_infra_config`
- `test_infra_storage`
- `test_infra_subprocess`
- `test_features_tools`
- `test_features_memory`
- `test_features_web`
- `test_features_channel`
- `test_app`
- `test_integration`

The exact partition can be adjusted slightly, but the final shape must keep modules understandable and keep unrelated tests out of the same process where possible.

### Use process-parallelism, not shared-process test parallelism

The primary speedup mechanism is:

- many test binaries
- each test binary runs independently
- the test runner executes those binaries in parallel

This is intentionally safer than trying to make one huge process run highly parallel test cases while sharing environment variables, default loggers, and temp roots.

### Centralize test isolation helpers

Add shared support under `tests/support` to provide:

- unique per-test temp roots
- unique per-test workspace roots
- unique DB file paths
- fixture copy helpers
- scoped environment variable helpers
- scoped default logger helpers
- helpers for deterministic cleanup

Tests must stop creating deterministic shared locations such as:

- fixed names under `tmp/tests/...`
- fixed files in `/tmp`
- reusable DB filenames that differ only by process ID

Every test that touches disk or process-global state should get its own isolated namespace.

### `boost-ext::ut` migration strategy

The testing rewrite should keep most assertions behaviorally equivalent while changing the framework style:

- simple `TEST(...)` cases become `ut` test blocks
- fixtures move to local helper structs/functions and scoped setup objects
- `ASSERT_*` vs `EXPECT_*` semantics are converted carefully so early-exit assumptions are preserved where needed
- `GTEST_SKIP()` cases are converted to explicit `ut` skip behavior or rewritten so the dependency on skip disappears

The goal is not a cosmetic 1:1 macro rewrite. The goal is smaller, clearer, isolated module tests that happen to use `boost-ext::ut`.

### Hashline tests should assert the new semantics

Where tests currently rely on hard-coded `xxhash`-derived constants, rewrite them around the new design rather than preserving obsolete expectations.

Tests should verify:

- determinism
- anchor parsing and validation under the new format
- edit application behavior
- collision handling semantics when relevant

## File-Level Design

### Build files

- Add `xmake.lua`
- Add `xmake/*.lua` support files
- Remove `CMakeLists.txt`
- Update [scripts/build-all.sh](/home/huxint/projects/orangutan/scripts/build-all.sh) to build through `xmake`

### Hashline

- `src/features/tools/core/hashline.cpp`
- `src/features/tools/core/hashline.hpp`

Introduce a narrow internal hashing helper and switch its implementation to `rapidhash`.

### REPL

- `src/app/repl.cpp`
- `src/app/repl.hpp`
- new small line-editor adapter files if needed

Swap the backend from `readline` to `replxx` without changing command semantics.

### Config secret crypto

- `src/infra/config/secret-protection-crypto.cpp`
- related config secret tests

Reimplement the crypto calls with `Mbed TLS` while preserving on-disk format compatibility.

### QQ transport

- `src/features/channel/qq/transport.cpp`
- `src/features/channel/qq/transport.hpp`
- possibly small new internal transport helper files

Replace the `Boost.Asio/Beast + OpenSSL` transport internals with a transport built on the chosen `libcurl + Mbed TLS` stack while keeping the public callback-facing API stable.

### Tests

- all `tests/**/*.cpp`
- `tests/test-helpers.hpp`
- new `tests/support/*`

Split the suite by module, replace framework usage, and remove shared-state assumptions.

## Migration Sequence

### Phase 1: `xmake` skeleton and target graph

- define packages, options, production targets, and test targets
- make the production binary build through `xmake`
- generate compile commands through `xmake`

### Phase 2: dependency and adapter replacements

- land the `rapidhash` adapter path
- land the `replxx` adapter path
- land the `Mbed TLS` config-crypto implementation
- land the QQ transport replacement

### Phase 3: test framework and isolation rewrite

- introduce shared isolation helpers
- migrate tests to `boost-ext::ut`
- split tests into module binaries
- enable parallel execution at the binary level

### Phase 4: final cleanup

- delete CMake files
- remove old dependency references from source and scripts
- ensure no `gtest`, `xxhash`, `readline`, or `OpenSSL` references remain
- ensure the documented developer workflow uses only `xmake`

Although implementation proceeds in phases, the repository is delivered only after the full cutover is complete.

## Validation Strategy

### Build validation

On Linux:

- `xmake f --toolchain=gcc`
- `xmake`
- `xmake f --toolchain=clang`
- `xmake`

Both toolchains must succeed.

### Test validation

- run the full module-split suite through `xmake`
- run it with default parallelism enabled
- confirm no failures caused by shared temp roots, environment collisions, logger collisions, or DB path reuse

### Behavioral validation

- config secret tests must prove the new format can encrypt and decrypt correctly
- hashline tests must prove deterministic anchors and valid edit behavior under the new design
- QQ tests must prove connect/reconnect/close behavior remains consistent at the interface level

### Removal validation

Searches should show no remaining functional dependency on:

- `cmake`
- `gtest`
- `xxhash`
- `readline`
- `openssl`

Residual mentions are acceptable only in historical documentation that does not affect the build or implementation.

## Risks And Mitigations

### Risk: `xrepo` package naming or options differ from expectations

Mitigation:

- use repository-local package overlays where needed
- pin versions in the lock file
- avoid unverified assumptions about backend defaults

### Risk: `libcurl + Mbed TLS` backend combination needs explicit control

Mitigation:

- make TLS/backend selection an explicit package concern in `xmake`
- if upstream package configuration is insufficient, provide a local package recipe for the required `libcurl` build

### Risk: test migration becomes a macro-by-macro rewrite with little improvement

Mitigation:

- require each migrated test module to adopt isolated temp and env helpers
- prefer helper extraction and fixture cleanup over literal framework translation
- split binaries by responsibility before chasing perfect stylistic parity

### Risk: hidden dependence on old hashline or secret formats

Mitigation:

- audit persisted fixtures and tests that assume old formats
- rewrite them against the new behavior instead of carrying compatibility shims
- keep format choices explicit in the new tests and docs

### Risk: one-shot migration hides regressions until late

Mitigation:

- validate each migration phase locally through `xmake`
- keep boundaries narrow around dependency-sensitive code
- end only when the full final state is already in place and verified

## Success Criteria

- The repository builds with `xmake` and no longer uses CMake.
- All third-party dependencies are managed through `xmake/xrepo` or repository-local `xmake` package overlays.
- Linux builds succeed with both `gcc` and `clang`.
- `xxhash`, `gtest`, `readline`, and `OpenSSL` are removed from the active implementation.
- `rapidhash`, `boost-ext::ut`, `replxx`, and `Mbed TLS` replace them successfully.
- QQ support remains present without relying on OpenSSL.
- The test suite is split into module-focused binaries and runs safely in parallel.
- Config secret protection works correctly with the new format and implementation.
- Hashline behavior works correctly under the new design, without carrying old-format compatibility requirements.
