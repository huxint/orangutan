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

**Enforcement:** `scripts/check-macros.sh` greps for forbidden patterns.

## C2. No `std::thread`, no custom thread pool

All concurrency goes through `oran::async::Runtime` (asio + coroutines). Background
work that needs the CPU pool uses `co_await async::post(runtime.cpu_executor(), ...)`.

**Why:** the runtime's cancellation, backpressure, and observability stories
depend on a unified executor. Bypassing it creates an island that none of those
stories serve.

**Enforcement:** `scripts/check-banned-includes.sh` rejects new `#include <thread>` in
non-test, non-bench code.

## C3. No exceptions across library boundaries

Public APIs return `core::Result<T> = std::expected<T, core::Error>`. Functions that
must throw (e.g., `main`-level bootstrap) catch at the boundary and translate to
`Result`.

**Why:** mixed exception/expected styles produce fragile error handling, partial
unwinding under coroutines, and hard-to-trace failures across library seams.

**Enforcement:** `scripts/check-throws.sh` walks `include/oran/**.hpp` and rejects
`throw` statements there. Plus a clang-tidy check (`cppcoreguidelines-no-throw`).

## C4. New SQLite code uses the expected API only

`oran-storage` exposes `Result<T>`-returning operations. The legacy throwing wrappers
(`must_ok`) **do not exist** in v2.

**Why:** mixed error models in `orangutan/` left ~120 callsites we couldn't migrate
cleanly. We will not repeat it.

**Enforcement:** the API surface does not include throwing wrappers, so there is
nothing to misuse.

## C5. Do not log or echo decrypted secrets

Secrets pass through `oran-config::SecretField` (read accessors). Their values are
**never** passed to `oran-log` directly; the redaction filter would also catch known
keys, but the source rule is: don't do it.

**Why:** legacy `orangutan/` shipped with no automatic redaction in the logger. We
added it to the v2 logger, but the cultural rule remains.

**Enforcement:** `scripts/check-secret-logs.sh` greps for known secret-field names
adjacent to `log::*` calls.

## C6. Public headers contain no heavy includes

Public headers under `include/oran/<lib>/` must not `#include` any of:

- `<nlohmann/json.hpp>` (use `<nlohmann/json_fwd.hpp>`)
- `<asio.hpp>` (use `<oran/async/awaitable_fwd.hpp>`)
- `<spdlog/spdlog.h>` (use `<oran/log/fwd.hpp>`)
- `<httplib.h>` (use `<oran/http/server_fwd.hpp>`)
- `<sqlite3.h>` (use `<oran/storage/handle_fwd.hpp>`)
- `<curl/curl.h>` (hide entirely)
- `<re2/re2.h>` (hide; expose `RuntimeRegex` opaque type)

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

## C9. clang-tidy / clangd warnings are errors

CI runs clang-tidy with the project's `.clang-tidy` config. Any warning fails the
build. Disabling a check at-site requires a comment with a justification.

**Why:** treating warnings as advisory is how legacy projects accumulate them by the
hundreds.

**Enforcement:** CI step `xmake check clang.tidy` (TBD with build skeleton).

## C10. Every effectful action is permissioned

Any code path that touches the filesystem, network, subprocess, memory store,
provider API, or another agent goes through `oran-permission::Evaluator` and
publishes a hook event. Bypassing this is a rule violation, not a shortcut.

**Why:** the permission story is what makes the runtime safe to use as a coding
assistant. Bypasses defeat it silently.

**Enforcement:** code review checklist + `scripts/check-bypass-permission.sh` (TBD).

## C11. Every async function is cancel-aware

Functions returning `async::Awaitable<T>` must either:

- Check `co_await asio::this_coro::cancellation_state` periodically, or
- Be composed entirely of awaitables that are themselves cancel-aware.

**Why:** SIGINT / shutdown must terminate promptly; orchestration cancellation must
work; user "stop" buttons must respond.

**Enforcement:** review checklist + bench: `bench/async/cancellation_latency` flags
> 250 ms median on a synthetic workload.

## C12. Every lib has a tests bucket AND a bench bucket

If a library exists under `src/oran-<lib>/`, then `tests/<lib>/` and `bench/<lib>/`
exist with at least one file each (an empty `placeholder.cpp` is acceptable
temporarily; an open issue is required to fill it).

**Why:** parity makes "is this covered?" mechanical. The bench bucket prevents the
"we'll add benches later" pattern that legacy never executed on.

**Enforcement:** `scripts/check-lib-parity.sh`.

## C13. Histories required for every code-change task

Behavior-changing tasks add a history entry under `docs/histories/YYYY-MM/`. See
[`../HISTORY_GUIDE.md`](../HISTORY_GUIDE.md).

**Why:** the next agent will want to know why a change exists; git log alone is
not enough.

**Enforcement:** PR template checklist + `scripts/check-history-touched.sh` flags PRs
that change code without a history file (overridable in PR description with a
`History-Skip: <reason>` trailer).

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

**Enforcement:** `scripts/check-pkgs-documented.sh` parses `xmake/packages.lua` and
fails if any package isn't in `libraries.md`.

## C16. Docs match reality — The Prime Directive

Every change to behavior, build, configuration, dependencies, interfaces, file
layout, commands, or conventions **must update the corresponding documentation in
the same PR**. A PR that ships code without updating the docs it invalidates is
*incomplete*. There is no "I'll update the doc later".

**Why:** the entire harness-engineering scaffold rests on the premise that
`docs/` is the system of record. The moment a doc lies, the next agent acts on a
lie. The legacy `orangutan/` had a `CLAUDE.md` referencing `.claude/rules/` files
that did not exist, code referencing tools that had been removed, and a half-done
migration whose status diverged between code and docs. We refuse to repeat that
pattern.

See [`docs-in-sync.md`](docs-in-sync.md) for the full mechanics, including the
change-type → docs-to-update mapping table.

**Enforcement:** `scripts/check-docs-sync.sh` (the rule's mechanical
enforcement — currently a stub; activates as the code base lands) plus review
checklist on every PR. The PR template's "Validation" section has a mandatory
"docs updated" checkbox.
