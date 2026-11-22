# Build System

The supported build uses C++26, GCC 16.1+ and xmake 3.1.1 on Linux. Current local
validation uses GCC 16.2.1. Source reflection requires this compiler baseline.

## Commands

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4

xmake build test-bootstrap
xmake run test-bootstrap

xmake f -y -m debug --sanitizers=y
xmake build -j4
xmake test -j4
```

The default build produces the runtime libraries. Tests and benchmarks are
separate targets: `test-<lib>` and `bench-<lib>`.
The opt-in `oran-telegram` application hosts a private Telegram conversation;
[messaging channels](design-docs/messaging-channels.md#telegram-deployment-host)
owns deployment commands, credentials and recovery. Build it with
`xmake build -j4 oran-telegram`; ordinary builds and tests never launch it.
The opt-in `oran-qq-login` executable provides official QR binding and saved-token
verification. Build it with `xmake build -j4 oran-qq-login` after installing system
`libqrencode` 4.1.1 development files. That optional dependency is isolated from
runtime libraries and tests. [QR authorization](design-docs/messaging-channels.md#qr-authorization)
owns the command and private credential contract.
The opt-in `eval-qq` target processes one trusted private QQ event for live
dialogue verification; [testing](rules/testing-and-bench.md#qq-live-dialogue-evaluation)
owns its command and recovery limits. The optional `scripts/qq-live.mjs`
Gateway evaluation harness uses Node 22+ built-ins and no npm packages.
`xmake test` builds test targets before running them.
The opt-in `eval-context` target runs controlled or explicitly configured live
working-context evaluations; [testing](rules/testing-and-bench.md#working-context-evaluation)
owns its options, output and interpretation. For a single Catch2 case,
invoke `build/linux/x86_64/release/test-<lib> "case name"` after building it.
Use the debug path when configured in debug mode.

`make help` lists repository maintenance commands. `make ci` checks repository
contracts and hygiene independently of C++ builds. Benchmarks use the ordinary
`bench-<lib>` build/run targets; compare their results under the same workload
and environment as described in [testing-and-bench](rules/testing-and-bench.md).

## Build Ownership

The optional React/TypeScript/Vite prototype lives in `apps/desktop` and uses
Node.js 24+ with a pinned npm lockfile. Its [README](../apps/desktop/README.md)
owns install, development, build and browser-test commands. Xmake does not fetch
frontend dependencies or build this prototype; `make ci` retains its repository
hygiene scope. Frontend changes run the app's build, format, state and browser
checks separately. No C++ toolchain is needed to preview the browser UI.

| File | Owns |
| --- | --- |
| `xmake.lua` | Language, common warnings, default toolchain and shared policy selection. |
| `xmake/targets.lua` | Runtime library graph. |
| `xmake/packages.lua` | Pinned fetched packages and required system libraries. |
| `xmake/toolchain.lua` | Compiler, assembler, linker and binutils discovery. |
| `xmake/build-policy.lua` | Shared project flags, reflection, LTO, sanitizers and analysis. |
| `xmake/options.lua` | Supported configure options. |
| `xmake/tests.lua`, `xmake/bench.lua` | Test and benchmark buckets. |
| `xmake/evals.lua` | Opt-in deployment-model evaluation executables. |
| `xmake/apps.lua` | Opt-in deployment hosts. |

The root selects `oran-gcc` and the `oran.build` rule for every runtime library,
test and benchmark. Tool discovery prefers `gcc-16`/`g++-16`, then `gcc`/`g++`;
the selected compiler must meet the supported baseline. Assembly uses the C
compiler driver for preprocessed dependency sources. The archiver prefers GCC's
LTO wrapper with the GNU ar interface required by xmake's Autoconf adapter.

| Option | Default | Effect on project targets |
| --- | --- | --- |
| `lto` | On | Link-time optimization in release mode. |
| `sanitizers` | Off | ASan/UBSan in debug mode, with frame pointers and recovery disabled. |
| `hardened` | Off | Fortify, stack protection, control-flow protection and stack-clash protection. |
| `analyze` | Off | GCC analyzer with the required warnings promoted to errors. |

LTO and sanitizers use xmake's policies to configure both compilation and final
linking. The shared rule applies the remaining project flags to sources and PCHs.
Dependency builds reuse tool discovery and retain their own package options;
project reflection, analysis and sanitizer flags stay in the project rule.
Fortify requires an optimized build. LeakSanitizer needs an environment that
permits its runtime checks.

Use ordinary configure/build/test targets for verification. When changing build
policy, inspect compiler and linker arguments with `xmake build -v` as well as
running tests; the configured option alone is not evidence of instrumentation.

The build uses headers and static libraries. `oran-channel` provides QQ, Telegram
and Feishu adapters using existing dependencies. No GUI or messaging SDK is required.

System libcurl development headers and pkg-config must be installed before
configuration. Xmake supplies Asio, Catch2, libsodium, nanobench, nlohmann_json,
RE2 and SQLite with FTS5. Exact versions and boundaries live in
[libraries](rules/libraries.md).

## Compile Cost

Public headers keep heavy third-party definitions private. Libraries use the
stable PCH in `include/oran/_pch.hpp`; changing a .cpp should rebuild its owning
archive and dependents' links. Optional packages must not affect default builds.

[compile-budget](rules/compile-budget.md) owns measurement thresholds;
[FAST_COMPILATION](FAST_COMPILATION.md) owns the measurement workflow. Hosted
functional builds are independent of unprovisioned reference-hardware gates.
