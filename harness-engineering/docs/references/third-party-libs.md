# Third-Party Libraries — Notes

Free-form notes that complement [`../rules/libraries.md`](../rules/libraries.md). The
rules file is the *approval list*; this file is *guidance + traps*.

## asio (standalone)

- Use the **standalone** variant (`asio::` namespace, no `boost::asio`).
- Coroutine integration is via `asio::awaitable<T>` and `asio::co_spawn`.
- Cancellation: pass `asio::cancellation_slot` or use `asio::detached` + a
  cancellation_signal you control.
- Compile-time cost is moderate. Hide `<asio.hpp>` from public headers via the
  `oran::async` wrapper.

## libcurl

- Use the multi handle (event-driven) interface, not `curl_easy_perform`.
- Integrate with the asio executor via `curl_multi_socket_action`.
- mbedtls option is **not** used in v2; we link against system OpenSSL via curl
  (`configs = { openssl = true }`).
- SSE: curl can stream; parse the events in `oran-http::sse::Parser`.

## sqlite3

- Compile with `-DSQLITE_ENABLE_FTS5`.
- WAL mode: `PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL`.
- One writer connection per DB (on a strand); pool of readers.
- Prepared-statement cache per connection.
- Migrations are SQL files under `src/oran-storage/migrations/<db>/`.

## libsodium

- v2 secrets primitive: `crypto_secretbox_easy` (XSalsa20-Poly1305).
- KDF: `crypto_pwhash` with Argon2id.
- Zeroization: `sodium_memzero` on plaintext buffers.
- Compile-time footprint is small; do not let it leak into `include/oran/`.

## re2

- Runtime regex; linear time guarantees.
- Patterns compiled once into `oran::permission::RuntimeRegex` and reused.
- Avoid features re2 doesn't support (back-references) in rule patterns.
- Hide `<re2/re2.h>` behind the wrapper type.

## spdlog + fmt

- Project-wide `SPDLOG_FMT_EXTERNAL=1`; use header-only fmt.
- Never include `<spdlog/spdlog.h>` from `include/oran/`; the `oran-log` shim is
  the only public surface.
- Levels match our enum (`trace, debug, info, warn, error`).
- Structured fields via `oran::log::field(...)`.

## nlohmann_json

- Use `<nlohmann/json_fwd.hpp>` in public headers.
- Full `<nlohmann/json.hpp>` only in `.cpp` files.
- For perf-critical paths (provider request encoding), consider simdjson; bench
  before adopting.

## cpp-httplib

- Encapsulated inside `oran-web`. Public surface does not mention it.
- Single-threaded event loop is a known limit (`docs/product-specs/0007-web-ui.md`).
- For client-side HTTP, **do not** use cpp-httplib; route through `oran-http::Client`
  (libcurl).

## ctre (legacy) → re2 (v2)

- ctre was used in legacy for compile-time regex (permissions, redaction).
- We replaced it because patterns now come from config; compile-time is impossible.
- ctre also added significant TU cost in some hot files.

## mbedtls (legacy) → libsodium + system OpenSSL (v2)

- mbedtls did TLS *and* secret crypto in legacy; the coupling forced both costs.
- v2 splits: TLS via system OpenSSL (curl), secrets via libsodium.

## stdexec-gtc (legacy) → asio coroutines (v2)

- The NVIDIA stdexec gtc-2026 fork was a custom dependency in legacy.
- Sender/receiver was overkill; type-erased `any_sender_of<...>` made compile-time
  and debugging painful.
- Replaced everywhere by `asio::awaitable<core::Result<T>>`.

## uni_algo (legacy) → folded into oran-core::str (v2)

- Only a few unicode-normalization paths used uni_algo; replaced by stdlib + simdutf
  helpers in `oran-core::str`.

## magic_enum

- Header-only; good for boundary stringification.
- For hot enums, define a manual `enum_name` overload to avoid magic_enum's per-enum
  compile cost on common iteration paths.
- Don't use `magic_enum::enum_values<T>()` at compile time for very large enums.

## simdutf

- Used by `oran-core::str::validate_utf8` and a few conversion helpers.
- Header is medium-cost; include behind a `.cpp` rather than a public header.

## rapidhash

- Use for non-crypto hashing only (cache keys, dedup).
- Not appropriate for security-sensitive contexts.

## replxx

- REPL line editor for `oran-cli`.
- Single-threaded; acceptable for our CLI use.
- Quirk: completion callbacks must not block on the event loop.

## Catch2 v3 + nanobench

- Catch2 is our test framework; nanobench is our benchmark framework.
- Catch2's BENCHMARK macro wraps nanobench when called from a test case.
- For dedicated bench binaries (`bench/<lib>/main.cpp`), prefer nanobench directly.

## See Also

- [`../rules/libraries.md`](../rules/libraries.md) — approval list.
- [`orangutan-legacy-audit.md`](orangutan-legacy-audit.md) — dependency disposition
  reasoning.
