# Libraries

This file is the **approval list** for third-party libraries. Every package in
`xmake/packages.lua` is documented here. Adding a new library requires adding an
entry.

Each entry contains:

- **Lib + version**
- **Used by** (which `oran-*` libraries)
- **Purpose**
- **Compile cost** (low / med / high — based on per-TU impact measurement)
- **License**
- **Alternatives considered**
- **Boundary** (where it may appear: public headers / `.cpp` only / single library)

## Approved Libraries

### Core

| Lib | Version | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- |
| `nlohmann_json` | 3.12.0 | many | JSON parsing/serialization | med | MIT | `.cpp` only; public headers use `json_fwd.hpp` |
| `fmt` | 12.1.0 | log, prompt, web | string formatting | low | MIT | PCH set |
| `spdlog` | 1.17.0 | oran-log only | structured logging | med | MIT | hidden behind `oran-log` shim |
| `magic_enum` | 0.9.7 | core | enum reflection | low | MIT | core only |
| `rapidhash` | 1.0 | core, storage | hashing | low | BSD-2 | wherever |
| `simdutf` | 8.0.0 | core::str | UTF-8 validation / transcoding | med | MIT | core::str only |

### Async / IO / Net

| Lib | Version | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- |
| `asio` | 1.31.0 | async, http | executor + coroutines + net | med | BSL-1.0 | `.cpp` only; public uses fwd shim |
| `libcurl` | 8.11.0 | http, channel-* | HTTP client | med-high | curl license | wrapped behind `oran-http::Client` |
| `cpp-httplib` | 0.37.2 | web only | HTTP server | med | MIT | `oran-web` only |
| `re2` | 2024.07.02 | log, permission, tool | runtime regex | med | BSD-3-Clause | hidden type `RuntimeRegex` |

### Storage / Crypto

| Lib | Version | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- |
| `sqlite3` | 3.52.0 | storage, memory, automation | embedded SQL | med | public domain | `oran-storage` only |
| `libsodium` | 1.0.20 | config | AEAD + KDF for secret-at-rest | low-med | ISC | `oran-config` only |

### CLI / UX

| Lib | Version | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- |
| `cli11` | 2.6.1 | bootstrap | CLI flag parsing | low | BSD-3-Clause | `oran-bootstrap` only |
| `replxx` | 2021.11.25 | cli | REPL line editor | low | BSD-3-Clause | `oran-cli` only |

### Test / Bench

| Lib | Version | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- |
| `Catch2` | 3.7.1 | tests/* | testing framework | med | BSL-1.0 | `tests/*` only |
| `nanobench` | 4.3.11 | bench/* | microbenchmark runner | low | MIT | `bench/*` only |

### Optional (Feature-Gated)

| Lib | Version | Option | Used by | Purpose | Cost | License | Boundary |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `sol2` | 3.5.0 | `--hook_lua=y` | hook | Lua hook sink | med | MIT | `oran-hook` only |
| `sqlite-vec` | 0.1.7-alpha | `--vector_memory=y` | memory | Vector search backend | low-med | MIT | `oran-memory` only |

## Removed vs. Legacy

| Lib | Why removed |
| --- | --- |
| `stdexec` (NVIDIA gtc-2026 fork) | Custom toolchain coupling; compile-time tax; sender/receiver was overkill for this codebase. Replaced by asio + coroutines. |
| `mbedtls` | Coupled secrets-crypto to TLS stack; we use system curl's TLS (OpenSSL) and libsodium for secrets. |
| `ctre` | Compile-time regex prevents config-driven patterns; replaced by `re2`. |
| `uni_algo` | Niche use; folded into `oran-core::str` using stdlib + simdutf. |

## Rules

### L1. New libraries require an entry here before the `xmake/packages.lua` edit

PR description includes:

- Why no existing library suffices.
- Compile cost estimate (measure on a synthetic TU).
- License compatibility note.
- The "boundary" — where in the codebase the library is allowed to appear.

### L2. Header-only deps that exceed 1 MB or 10 kLoC need extra justification

Examples passing the bar: `nlohmann_json`, `fmt`. Examples that did *not* pass:
`stdexec`. Future submissions get scrutinized.

### L3. Two libraries doing the same job is forbidden

If `oran-channel-discord` reaches for cpr while `oran-http` uses libcurl, that's a
red flag. Pick one HTTP client (libcurl) and route everything through it. The
"boundary" column makes this explicit.

### L4. Optional libraries must be **strictly opt-in**

A `--vector_memory=y` build pulls in `sqlite-vec`. A default build does not. The
compile-time impact of an optional library on a default build must be **zero**.

### L5. Update on bump

When a library bumps version, the entry in this file is updated in the same PR. CI
checks that `xmake/packages.lua` versions match `docs/rules/libraries.md`.

**Enforcement:** `scripts/check-pkgs-documented.sh`.

## Alternatives Considered (Decision Log)

### HTTP client: libcurl vs. cpr vs. asio-only

- **libcurl** chosen: mature, supports streaming + SSE, system-available, single
  proven dependency.
- **cpr**: wraps libcurl with a nicer API but adds a header-only layer we don't need.
- **asio-only HTTP**: would require writing our own SSE parser and TLS plumbing.

### JSON: nlohmann_json vs. simdjson vs. RapidJSON

- **nlohmann_json**: best UX, dominant in C++ projects. Header-only cost is mitigated
  by `json_fwd.hpp`.
- **simdjson**: faster parsing but DOM API less convenient; consider for a `simdjson`
  fast-path inside `oran-provider` if profiling shows benefit.
- **RapidJSON**: legacy choice; nlohmann_json's API ergonomics win.

### Crypto: libsodium vs. mbedtls vs. OpenSSL

- **libsodium**: small, single-purpose, easy to use correctly. Chosen for secrets.
- **mbedtls**: replaced; coupled to TLS.
- **OpenSSL**: used implicitly via libcurl for TLS.

### Regex: re2 vs. std::regex vs. PCRE2 vs. ctre

- **re2**: linear-time guarantees, runtime-configurable, small TU. Chosen.
- **std::regex**: slow at runtime, slow at compile time, sometimes outright wrong.
- **PCRE2**: feature-rich but heavier; not justified for our needs.
- **ctre**: replaced; can't accept config-driven patterns.

### Async: asio vs. stdexec vs. coroutines-only

- **asio + coroutines**: chosen. Mature library, single executor, asio coroutines play
  well with `co_await`.
- **stdexec**: the legacy choice; complex sender/receiver was overkill and compile-time
  expensive.
- **coroutines without a library**: would require writing schedulers / executors /
  cancellation infrastructure from scratch.
