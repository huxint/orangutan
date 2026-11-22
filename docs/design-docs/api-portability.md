# Provider Boundary

`provider::System::send` performs one attempt over a typed request and a selected
`ModelTarget`, returning `core::Result<Response>`. HTTP transport is injected.
`provider::execution::run` owns route traversal, retries, attribution and pricing;
the agent loop calls that boundary over its borrowed backend.

## Construction

Bootstrap's `resolve_route_profiles` converts configured names, aliases and model
policy into owned endpoint values. The provider library accepts
`RouteProfileResolution` and `SecretLookup` from `protocol_transport.hpp`.
[Runtime composition](bootstrap-runtime.md) owns configuration conversion.

`make_protocol_system` validates the complete route before reading credentials:
endpoint fields are nonempty, URLs use HTTP/S, protocols are implemented and
profile names are unique. An invalid fallback fails before primary credential
lookup. Lookup failures expose only profile, role and credential-reference
context.

The returned system owns immutable endpoint credentials and borrows one
`ProtocolTransport`. Each send validates the selected profile, model and protocol
before transport work. Concurrent requests own their decoder state. The
transport, system and event sink remain alive until awaited sends, callbacks and
cancellation cleanup complete.

## Execution Outcomes

`execution::run(backend, request, route, sink)` returns an owned `Outcome`:
`target` identifies profile, model, protocol and whether a fallback was selected;
`response` is the single `core::Result<Response>` failure channel. Attribution
exists on both success and failure, including invalid attempt budgets and
cancellation. It comes from execution's selected target, never from an optional
response profile or an error-string lookup.

A successful endpoint's nonempty reported model becomes the attributed model;
an omitted model uses the configured name. Failure identifies the attempted
target. Error context remains diagnostic and cannot redirect hook or trace
attribution. The loop consumes this value directly, accumulates usage and
publishes response/error/fallback observations.

Each route target receives `Request::retry.max_attempts` attempts; zero is invalid.
Retryable failures exhaust the current target before advancing through fallbacks.
Non-retryable errors and cancellation return immediately. A delivered stream
callback prevents replay. Backoff respects the greater of `retry_after` and the
configured initial delay. Fallbacks apply their own thinking and cache policy.
Unexpected backend exceptions become internal errors without exposing exception
text; Asio operation-aborted exceptions become cancellation.

Successful usage keeps any endpoint-supplied `cost_estimate`, including zero.
Otherwise execution estimates cost from the selected profile's token prices.
Missing output/cache prices use input pricing; an absent input price contributes
zero. If every price is absent, cost remains unknown. The loop does not resolve
profiles or recalculate prices.

## Protocols And Streaming

Implemented protocols are `anthropic_messages` and `openai_responses`.
User image blocks carry a JPEG, PNG, GIF or WebP media type and inline base64.
Anthropic encodes an `image` block with a base64 source; Responses encodes
`input_image` with a data URL. Provider credentials and remote download URLs
are never part of image content. The configured model must support vision;
protocol support alone does not imply that every model accepts images.
`HttpProviderBackend` owns the transport, system and resolved route.
Adapters translate roles, content, tool calls/results, stop reasons and usage,
and classify malformed responses and provider errors. Anthropic-format requests
send `thinking.type=disabled` when no thinking budget is configured; omission is
not equivalent on endpoints such as DeepSeek, which enable thinking by default.
An explicit budget sends `thinking.type=enabled` with `budget_tokens`. This keeps
bounded summary requests from spending their output allowance on reasoning.

Both protocols send the tool result's nonempty model-facing text verbatim,
including paging and incomplete-output notices. Only an empty text field falls
back to the structured JSON string. Structured data remains in the typed result
for hosts and persistence; it must not replace instructions carried in text or
duplicate the file body on the model wire.

HTTP/SSE transport enforces deadlines and byte caps. Decoders assemble text/tool
input incrementally and reject inconsistent or incomplete terminal events.
A partial stream is an observation, not a completed persisted answer.

`http::Client` owns unique curl easy/multi handles and a header list on the worker
executor. A guard detaches the easy handle before release. Bounded multi polling
observes cancellation. Pending work retains request values, cancellation state
and executor work until completion. SSE callbacks run serially on the caller's
executor and finish before send returns.

## Prompt Caching

The loop forwards current `prefix_hash` and `prefix_bytes` values for its owned
system text and native declarations. Retries and fallbacks retain these inputs.
Provider execution has no prompt-rendering or section-layout dependency.
Callers keep identity aligned with submitted stable fields; conversation,
including system messages in `Request::messages`, is separate.

`make_protocol_request` applies the selected target's cache policy to every
attempt. Absent policy enables caching with a zero-byte floor. Missing hints, a
zero-byte prefix, disabled policy, a prefix below its byte floor, or no stable
system text/native tools omit explicit controls. Equality with the floor is
eligible. One target's policy cannot suppress controls for another target.

| Protocol | Explicit control |
| --- | --- |
| `anthropic_messages` | Stable system text ends in a `cache_control: {"type":"ephemeral"}` block. With no stable system text, the last native tool receives the breakpoint. System messages lifted from conversation follow it; conversation and tool results receive no markers. |
| `openai_responses` | `prompt_cache_key` is `oran-` followed by the current prefix hash as 16 lowercase hexadecimal digits. |

These controls follow [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)
and [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
Disabling explicit controls does not disable OpenAI's automatic caching. Byte
eligibility is local policy, not a provider token threshold. Controlled tests
establish request encoding and route behavior, not service cache hits.
[Prompt design](../rules/prompt-design.md) owns current text and fingerprint rules.
