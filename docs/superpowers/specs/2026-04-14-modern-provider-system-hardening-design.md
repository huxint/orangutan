# Modern Provider System Hardening Design

**Date:** 2026-04-14

**Status:** approved for planning

## Goal

Complete the provider-system refactor as a clean first-generation architecture. The result should be modern, strict, and minimal: one provider schema, one fluent request API, clear protocol boundaries, efficient streaming, and no compatibility layer for legacy provider interfaces or config shapes.

## Scope

In scope:

- `src/config/config.cpp`
- `src/config/config-sections-core.cpp`
- `src/bootstrap/bootstrap.cpp`
- `src/bootstrap/cli-options.cpp`
- `src/providers/provider.hpp`
- `src/providers/provider.cpp`
- `src/providers/execution/runtime-backend.cpp`
- `src/providers/transport/http-transport.hpp`
- `src/providers/transport/http-transport.cpp`
- `src/providers/protocols/openai-chat-completions.cpp`
- `src/providers/protocols/openai-responses.cpp`
- `src/providers/protocols/anthropic-messages.cpp`
- `config.example.json`
- focused provider/config tests

Out of scope:

- reintroducing `Provider`, `ProviderEndpoint`, `chat()`, or `chat_stream()`
- accepting or translating legacy `endpoint_style`
- broad unrelated cleanup outside provider/config/runtime boundaries

## Problems To Address

### 1. The configuration boundary is still soft

The new provider model is intended to be the only public shape, but the current config-loading path still behaves like a migration layer in practice. Invalid provider config can be collapsed into an empty config object, which delays failure and makes startup behavior ambiguous.

### 2. Execution fallback depends on lower layers behaving perfectly

`RuntimeBackend` currently retries only when lower layers throw `ProviderError`. Protocol adapters and decoders can still leak raw parsing exceptions, which lets malformed upstream responses bypass fallback handling entirely.

### 3. Streaming transport keeps the whole response in memory

The new SSE path parses events incrementally but still appends every streamed chunk into a success-path body buffer. That creates unnecessary allocation and copy costs on the hottest provider path.

### 4. Layer responsibilities are close, but not fully sealed

The refactor already separates route selection, protocol selection, auth, transport, and streaming events. The remaining work is to make those boundaries explicit enough that each layer has one job and a predictable failure contract.

## Design

### Configuration model: strict first-generation schema

The configuration model is the new public contract and should not carry legacy migration logic.

Rules:

- model configuration accepts only `provider`, `protocol`, `max_tokens`, `context_window`, `thinking`, and `cost`
- `provider` and `protocol` are required
- invalid enum values are rejected immediately
- legacy `endpoint_style` is neither parsed nor detected specially

If a config file exists but contains invalid provider configuration, loading must fail immediately. The config layer should not convert schema errors into an empty `Config` value. This keeps failures early, deterministic, and visible at the real boundary where the mistake exists.

### Startup behavior: fail fast and stay explicit

Startup should treat malformed config as a fatal error.

`Config::load_from()` should keep the existing "missing file returns defaults" behavior, but once a file is present and successfully parsed as JSON, structural config errors must propagate as exceptions. The bootstrap and config-protection entry points should catch those exceptions, print a clear message, and exit with a failure code.

This keeps the runtime model simple:

- no hidden defaulting for broken provider definitions
- no "loaded successfully but failed later" agent resolution path
- no compatibility fallback for old provider config shapes

### Request pipeline: fluent API remains the single entry point

`ProviderSystem` and `RequestBuilder` remain the only provider-facing API.

The target request flow stays:

`ProviderSystem.route(...).system(...).messages(...).tools(...).stream().on_event(...).send_blocking()`

The chain should remain a thin request-construction layer that forwards a typed `ProviderRoute`, `ProviderRequest`, and optional event sink into `ProviderBackend`. No old chat-style helper API should be reintroduced anywhere in runtime code or tests.

### Runtime backend: fallback policy only

`RuntimeBackend` is responsible for:

- flattening and deduplicating route targets
- tracking preferred target, active target, and usage counters
- deciding whether a failure is retryable
- enforcing "do not fallback after output has already streamed"

`RuntimeBackend` is not responsible for understanding JSON formats, vendor-specific payloads, or raw libcurl error types. It should operate on normalized `ProviderError` values.

Implementation strategy:

- lower layers should convert all expected failures into `ProviderError`
- `RuntimeBackend` should still keep a narrow outer catch for leaked `std::exception` and normalize it into `ProviderError{error_category::unknown, ...}`
- retry/fallback decisions continue to use `ProviderError::retryable()`

This preserves a simple execution core while making the fallback path robust against future adapter bugs.

### Protocol adapters: own protocol shape and exception normalization

Each adapter owns two things:

- request/response translation for one provider/protocol pair
- exception normalization for that provider/protocol pair

That means:

- `build_request(...)` throws `ProviderError` for invalid request shaping
- `parse_response(...)` throws `ProviderError` for malformed or invalid upstream payloads
- `StreamDecoder::on_event(...)` and `finish()` throw `ProviderError` for malformed streamed payloads or invalid accumulated state

Raw `nlohmann::json` exceptions should not escape adapters. The adapter layer is where third-party protocol irregularities become first-party runtime semantics.

Recommended classification:

- malformed JSON or missing required payload structure -> `error_category::parsing`
- valid JSON with clearly invalid upstream semantics -> `error_category::upstream` or `error_category::unknown`

### HTTP transport: separate buffered and streaming contracts

Transport should expose two distinct operations because they have different performance characteristics and different caller expectations.

Buffered path:

- `post(...) -> HttpResponse`
- returns full body for non-streaming requests

Streaming path:

- `stream_sse(...) -> void`
- feeds parsed event chunks to a callback
- does not retain the full successful response body

For streaming errors, transport may keep a very small bounded preview buffer only for non-2xx response diagnostics. Successful SSE responses should not allocate proportional to output length.

This produces a cleaner transport abstraction and removes unnecessary work from the hot path.

### SSE handling: incremental by default

`SseParser` remains incremental and event-oriented. The transport layer should feed chunks directly into the parser, and protocol decoders should consume event names plus payload slices without any success-path whole-body assembly.

This preserves a streaming-first design:

- minimal allocations
- no duplicate buffering
- natural support for long text or reasoning streams

## Testing Strategy

### Config tests

Add or update tests to prove:

- missing `provider` fails config loading
- missing `protocol` fails config loading
- invalid `provider` / `protocol` values fail config loading
- config files with structural provider errors do not collapse into an empty config

### Protocol tests

Add focused tests for each adapter to prove:

- malformed JSON in `parse_response(...)` becomes `ProviderError`
- malformed streamed payloads in decoders become `ProviderError`
- valid responses still parse into the expected `LLMResponse`

### Execution tests

Add or update tests to prove:

- retryable pre-output failures fallback to the next target
- non-retryable failures do not fallback
- any failure after at least one streamed event does not fallback
- leaked non-`ProviderError` exceptions are normalized and do not crash fallback policy

### Transport tests

Add focused coverage for the streaming transport contract:

- SSE events are delivered incrementally
- successful streaming requests do not accumulate the entire body
- non-2xx streaming responses still surface useful bounded diagnostics

### Regression set

At minimum run focused suites covering:

- config parsing
- provider core
- protocol adapters
- provider-system execution
- SSE parser / transport behavior

If implementation changes affect broader bootstrapping behavior, also run the relevant bootstrap and CLI tests that exercise config loading and runtime assembly.

## Acceptance Criteria

- provider config uses only the new schema and fails fast when invalid
- `Config::load_from()` no longer hides provider-schema errors behind an empty config
- provider runtime code uses only `ProviderSystem` and `RequestBuilder`
- adapters and decoders normalize protocol/parsing failures into `ProviderError`
- runtime fallback remains robust even if a lower layer leaks a generic exception
- streaming transport no longer buffers the full successful SSE response body
- focused provider/config tests pass with the new strict contracts
