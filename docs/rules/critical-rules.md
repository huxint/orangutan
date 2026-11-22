# Critical Rules

The non-negotiable list. Read this **before any code edit**. Each rule has a rationale
and an enforcement hook.

## C1. No macros for control flow

`#define` is allowed only for include guards and platform-conditional compilation
(`#ifdef _WIN32`). Anything that hides flow control (`#define TRY(x) ...`, `RETURN_IF_ERR`,
`UNWRAP`) is forbidden — write the explicit `if (!result) return std::unexpected(...)`.

**Why:** macros invisibly break tooling (clangd, debugger, refactoring), they bypass type
checking, and they grow into a private dialect. We have `std::expected`, concepts,
and templates for the same job.

**Enforcement:** code review; no automated macro check is installed.

## C2. No `std::thread`, no custom thread pool

All concurrency goes through `oran::async::Runtime` (asio + coroutines). Background
work that needs the CPU pool uses `co_await async::post(runtime.cpu_executor(), ...)`.

**Why:** the runtime's cancellation, backpressure, and observability stories
depend on a unified executor. Bypassing it creates an island that none of those
stories serve.

**Enforcement:** code review covers source includes. `scripts/check-includes.sh`
rejects `<thread>` in public headers as part of `make ci`.

## C3. No exceptions across library boundaries

Fallible APIs use `core::Result<T> = std::expected<T, core::Error>`. A terminal
domain value may pair this result with metadata shared by success and failure;
`Result` remains the single failure channel. Functions that must throw catch at
the boundary and translate to `Result`.

**Why:** mixed exception/expected styles produce fragile error handling, partial
unwinding under coroutines, and hard-to-trace failures across library seams.

**Enforcement:** code review of public APIs; no automated throw check is installed.

## C4. SQLite code uses the expected API only

`oran-storage` exposes `Result<T>`-returning operations. Callers inspect or
propagate failures before starting dependent work.

**Why:** one error channel keeps coroutine cleanup and transaction boundaries
explicit.

**Enforcement:** storage APIs return results; code review checks propagation.

## C5. Do not expose credential values

Configuration stores provider credential references such as `api_key_env`.
Provider construction resolves values through the named environment variable or
an injected `SecretLookup`. Keep values out of errors, hooks, traces and logs;
diagnostics identify only the profile and credential reference.

**Why:** diagnostics can outlive the request and be visible beyond the credential's
intended boundary.

**Enforcement:** `scripts/check-secret-logs.sh` checks known secret names in logger
calls; review and credential-failure tests cover errors and diagnostics.

## C6. Public headers contain no heavy includes

Public headers under `include/oran/<lib>/` must not `#include` any of:

- `<nlohmann/json.hpp>` (use `<nlohmann/json_fwd.hpp>`)
- `<asio.hpp>` (use `<oran/async/awaitable_fwd.hpp>`)
- `<spdlog/spdlog.h>` (hide; the logging surface is a library-local detail)
- `<httplib.h>` (hide; `oran-http` exposes its own boundary types)
- `<sqlite3.h>` (hide; `oran-storage` owns the handle)
- `<curl/curl.h>` (hide entirely)
- `<re2/re2.h>` (hide; expose `RuntimeRegex` opaque type)

A library that needs to expose an opaque type whose implementation pulls a
heavy include adds its own `<oran/<lib>/<name>_fwd.hpp>` header,
`<oran/async/awaitable_fwd.hpp>` being the current exemplar; a new public
header that cannot avoid a listed include must not be added.

**Why:** see `docs/FAST_COMPILATION.md`.

**Enforcement:** `scripts/check-includes.sh`.

## C7. Implicit conversions are off

- `explicit` on single-argument constructors.
- No implicit `bool` conversions from non-bool types.
- No comma operator overloads.
- No user-defined conversion operators except in `oran-core`'s strong-typedef helpers.

**Why:** silent conversions are a recurring source of misbehavior; the explicitness
overhead is small.

**Enforcement:** clang-tidy `google-explicit-constructor`, plus review.

## C8. RAII for everything

No `new`/`delete` outside `unique_ptr::make` / `shared_ptr::make`. No bare `malloc`.
Resources (file descriptors, sockets, sqlite handles, libcurl handles, mutexes) live
in RAII wrappers.

**Why:** exception/coroutine safety; preventing leaks under partial unwinding.

**Enforcement:** clang-tidy `cppcoreguidelines-owning-memory`,
`cppcoreguidelines-no-malloc`.

## C9. clang-tidy / clangd warnings are errors when checked

The project requires clang-tidy findings to be treated as errors. Hosted CI does
not run that gate yet; the missing job/config is tracked in
[live debt](../exec-plans/tech-debt-tracker.md). When clang-tidy is run locally or
in the future hosted job, any warning fails the check. Disabling a check at-site
requires a comment with a justification.

**Why:** treating warnings as advisory is how legacy projects accumulate them by the
hundreds.

**Enforcement:** planned hosted `xmake check clang.tidy` step; currently review
plus local/editor analysis.

## C10. External tool effects are permissioned; internal state is scoped

Tool effects on workspace files, external network services, subprocesses and
loaded executable code go through `permission::evaluate` and publish hook events.
Internal memory, task bookkeeping and dynamic child coordination instead use
explicit runtime dispatch: host-bound scope, functional enablement, validation,
admission limits and domain hooks. They do not enter generic allow/deny/ask.
Child agents inherit constraints on their external effects; delegation cannot
bypass them. Provider and storage services remain explicit host-owned resources,
not permission prompts for internal housekeeping.

**Why:** authority belongs at the boundary of the actual external effect.
Internal coordination remains usable without an approval consumer while
preserving ownership and user data.

**Enforcement:** code review and behavioral regressions for authorization and
denied effects.

## C11. Every async function is cancel-aware

Functions returning `async::Awaitable<T>` must either:

- Check `co_await asio::this_coro::cancellation_state` periodically, or
- Be composed entirely of awaitables that are themselves cancel-aware.

**Why:** SIGINT / shutdown must terminate promptly; orchestration cancellation must
work; user "stop" buttons must respond.

**Enforcement:** review checklist. A dedicated cancellation-latency bench is planned
for the first scheduler/orchestration workload where latency has operational meaning.

## C12. Every lib has a tests bucket AND a bench bucket

If a library exists under `src/oran-<lib>/`, then `tests/<lib>/` and `bench/<lib>/`
exist with at least one file each (an empty `placeholder.cpp` is acceptable
temporarily; an open issue is required to fill it).

**Why:** parity makes "is this covered?" mechanical. The bench bucket prevents the
"we'll add benches later" pattern that legacy never executed on.

**Enforcement:** `scripts/check-docs-sync.sh` checks source buckets in `make ci`;
`scripts/check-lib-parity.sh` checks the target inventory. Review verifies that
the buckets contain meaningful coverage.

## C13. Durable rationale belongs with the contract

Record architectural rationale in the design/rule/spec that owns the current
contract. Do not create per-change narrative files, duplicate test counts, or a
parallel release ledger; commit messages and Git history are the archive.

**Why:** manually synchronized ledgers drift and hide the documents that actually
govern the runtime. A future contributor needs the current invariant and its
rationale, not a second changelog.

**Enforcement:** review rejects stale current-contract docs and unnecessary ledgers.

## C14. PRs ≤ 600 lines / 6 files when possible

Larger changes need an execution plan first. The numbers are guidelines, not hard
caps, but they're enforced in CI as warnings; explicit override required for
exceptions.

**Why:** review fidelity drops sharply past these sizes. Plans first, code second.

**Enforcement:** PR-template prompt + CI warning.

## C15. No silent dependencies

A new third-party library requires an entry in [`libraries.md`](libraries.md) with
rationale, license, compile-cost estimate, and the libraries that depend on it.

**Why:** dependency creep is the second-biggest contributor to compile-time bloat
(after include hygiene), and a security surface in its own right.

**Enforcement:** `scripts/check-docs-sync.sh` checks package names and pinned
versions against `libraries.md` as part of `make ci`.

## C17. Language standard is C++26; modern facilities preferred

The project compiles as **C++26** under **GCC 16.1**. `set_languages("c++26")` is
the contract in `xmake.lua`; lowering it requires an exec plan and a rule edit.

Concretely:

- Use `std::print` / `std::println` / `std::format` instead of `<iostream>`. The
  `<iostream>` header is not allowed outside `tests/` and `bench/` runners, and even
  there it should be avoided when `std::print` works.
- Use `std::expected<T, Error>` (aliased as `core::Result<T>`) for fallible APIs.
- Use `std::generator<T>` instead of hand-rolled iterator pairs for lazy sequences.
- Use `std::span<const T>` instead of pointer + length pairs in interfaces.
- Use deducing-`this` where a non-virtual member benefits from forwarding-reference
  qualification.
- Prefer `consteval` / `constinit` over template-metaprogramming workarounds where
  GCC 16.1 lets you.
- **Prefer existing library functions over hand-rolled equivalents.** Before writing a
  raw `for` loop, a manual search, a hand-coded transform, or your own min/max/sort
  helper, check the standard library (`<algorithm>`, `<ranges>`, `<numeric>`,
  `<bit>`, `<charconv>`, `<chrono>`) and the in-repo libraries (`oran-core`,
  `oran-async`, `oran-io`, …) for a function that already does it. Reach for an
  in-repo helper before adding a new one.
- **Prefer `std::ranges` over the unprojected `std::` algorithms.** Use
  `std::ranges::find_last_if(rng, pred)` instead of a reverse-iterator loop, and
  `std::ranges::sort(rng)` instead of `std::sort(rng.begin(), rng.end())`. The
  range-based forms are clearer at the callsite, project-aware via projections,
  and avoid begin/end pairs.
- **Use `contains` for membership tests, not `find != end()` / `find != npos`.**
  `std::ranges::contains(rng, x)`, `map.contains(key)`,
  `std::string::contains(sub)` are C++20/23 first-class membership tests. The
  iterator-form `find` is reserved for callsites that actually use the
  iterator after the check; spelling a pure membership check with `find` is a
  review-blocking style violation. See
  [`code-style.md` "Membership Tests"](code-style.md).
- **Use reflection for enum<->string mappings.**
  `core::enum_name(value)` / `core::parse_enum<E>(text)` /
  `core::enum_values<E>()` from
  [`include/oran/core/enum_names.hpp`](../../include/oran/core/enum_names.hpp)
  are the one place every enum's wire spelling lives. New `enum class`
  declarations *do not* get a hand-maintained string table, and *do not*
  get a per-enum `to_string_view`/`parse_<kind>` forwarding shim — callers
  use the generic helpers directly. Only enums whose wire format deviates
  from the identifier (dashes, alternate casing) keep a hand-written
  switch. See [`code-style.md` "Enums"](code-style.md).
- **Prefer the newer facility when two equivalents exist.** If the standard ships
  both an older and a newer version of a function or type covering the same use
  case (e.g., `std::format` vs. `sprintf`, `std::filesystem::path` vs. raw
  strings, `std::span` vs. pointer-and-length, `std::optional` vs. sentinel
  values, `std::variant` vs. tagged unions, `std::ranges::*` vs. iterator-pair
  `std::*`, `std::ranges::contains` vs. `find != end()`), use the newer one
  unless there is a benchmarked reason not to.
- **No C-style constructs when a C++ equivalent exists.** Concretely: use
  `static_cast` / `reinterpret_cast` / `std::bit_cast` not `(T)x`;
  `static_cast<void>(expr)` not `(void)expr`; `nullptr` not `NULL` or `0`;
  `using` aliases not `typedef`; `enum class` not bare `enum`; `std::array` /
  `std::span` not raw `T[N]` in interfaces; RAII / smart pointers not
  `malloc` / `free`; `std::string` / `std::string_view` not `char*` / `strlen`
  in public APIs; `std::print` / `std::format` not `printf` / `sprintf`;
  `<cstring>` byte ops only behind a typed helper, never as the surface.
  Code should read as **simple, efficient, elegant, modern C++** — see
  [`code-style.md` "C++ Over C Idioms"](code-style.md).

**Why:** the rewrite exists specifically because the legacy project's older standard
left it stuck with hand-rolled equivalents of `std::expected` and friends. We use
what the toolchain ships.

**Enforcement:** `xmake.lua` pins `c++26`; the shared `oran.build` rule enables
reflection. Source review enforces the facility choices and the `<iostream>`
restriction.

## C18. Static analysis is on the menu, not the autopilot

GCC 16.1's `-fanalyzer` is wired into the build via `xmake f --analyze=y`. The
analyzer is **opt-in** because it triples compile time on heavy TUs (see
[`compile-budget.md`](compile-budget.md)). The intended nightly gate is mandatory
for covered memory/descriptor TUs, but hosted analyzer coverage is not active yet;
the gap is tracked in [live debt](../exec-plans/tech-debt-tracker.md). Suppression
policy and the required warning set live in [`static-analysis.md`](static-analysis.md).

**Why:** legacy `orangutan/` accumulated null-deref / use-after-free shapes the
analyzer would have caught; the build wiring exists so the missing hosted gate
can be activated without redesigning the toolchain.

**Enforcement:** `xmake f --analyze=y && xmake` locally and in the planned
nightly job. Suppressions without an explaining comment fail review.

## C16. Docs match reality — The Prime Directive

Every change that invalidates a documented public behavior, build/configuration
contract, interface, or architectural invariant **must update that owning document
in the same PR**. Internal refactors do not require ceremonial doc churn.

Canonical scope and enforcement live in [`docs-in-sync.md`](docs-in-sync.md).

**Enforcement:** `scripts/check-docs-sync.sh` plus review of the affected contract.
