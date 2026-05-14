# API Portability

This document describes the provider abstraction: how the runtime sends a domain
`provider::Request` to *some* LLM, *some* protocol, *some* transport, and how the
result comes back as a `provider::Response`. The legacy `orangutan/` design (transport
→ protocol → execution) was sound; v2 keeps the shape but slims the surface and removes
the `stdexec` dependency.

## Domain Model

```cpp
namespace orangutan::provider {

struct Request {
  std::vector<core::Message> messages;
  std::optional<std::string> system_prompt;
  std::vector<core::ToolDef> tools;
  std::optional<std::string> tool_choice;     // "auto", "any", "<name>"
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::uint32_t> thinking_budget;
  bool                       stream = true;
  std::optional<CacheHints>  cache;
  RetryPolicy                retry;             // execution-layer concern
};

struct Response {
  std::vector<core::Content> blocks;            // text, tool_use, thinking, ...
  StopReason                 stop_reason;
  Usage                      usage;             // input, output, cache tokens
  std::optional<ModelTarget> model_used;        // for fallback observability
};

}  // namespace orangutan::provider
```

`core::Content` is a typed variant; protocol adapters translate to/from vendor JSON.

## Layered Implementation

```
┌──────────────────────────────────────────────────────────────┐
│ oran-provider::System (public entry)                          │
│   Request → Response                                          │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ execution::Runtime                                            │
│   retry, fallback model, usage aggregation, cost tracking    │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ protocol::Adapter (one per API shape)                         │
│   anthropic_messages, openai_chat_completions,                │
│   openai_responses, gemini, custom_openai_compatible          │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ transport (asio-based http + SSE)                             │
│   oran-http::Client (HTTP, SSE, streaming bodies)             │
└──────────────────────────────────────────────────────────────┘
```

### Public Entry

```cpp
class System {
 public:
  System(execution::Runtime&, ProviderRegistry&);

  // Single-shot blocking send (returns when stop_reason is terminal).
  async::Awaitable<core::Result<Response>>
  send(Request, Route, EventSink* sink = nullptr) const;
};
```

`Route` is the resolved primary + fallbacks. `EventSink` is the streaming hook: the
adapter calls `sink->on_text_delta`, `sink->on_thinking_delta`,
`sink->on_tool_start`, `sink->on_tool_delta`, `sink->on_done` as the stream progresses.

### Routes

```cpp
struct Route {
  ModelTarget                 primary;
  std::vector<ModelTarget>    fallbacks;
};

struct ModelTarget {
  std::string profile;              // config-defined profile key
  std::string model;                // vendor model id
  ProtocolKind protocol;            // enum: anthropic_messages, openai_chat, …
  std::optional<std::uint32_t> thinking_budget;
  std::optional<CacheHints> cache;
  Capabilities caps;                // streaming, tool_use, thinking, vision, …
};
```

Profiles live in config; routes can be defined globally or per-agent. The agent's
`Loop` resolves a `Route` once per turn (or once per `Loop` if static) and reuses it
across iterations.

### Protocol Adapters

```cpp
class Adapter {
 public:
  virtual ~Adapter() = default;

  virtual Capabilities capabilities() const = 0;

  virtual async::Awaitable<core::Result<Response>>
  send(http::Client&, const Profile&, const ModelTarget&, const Request&,
       EventSink* sink) = 0;
};
```

Built-in adapters:

| ProtocolKind                   | Vendor format                                  | Notes |
| ------------------------------ | ---------------------------------------------- | ----- |
| `anthropic_messages`           | Anthropic Messages API (`/v1/messages`)        | Streaming, extended thinking, tool_use |
| `openai_chat_completions`      | OpenAI Chat Completions (`/v1/chat/completions`) | Streaming, function calling |
| `openai_responses`             | OpenAI Responses API (`/v1/responses`)         | Streaming, multimodal |
| `gemini_generate_content`      | Google Gemini (`/v1beta/models/.../generateContent`) | Streaming via SSE |
| `deepseek_chat`                | OpenAI-compatible                              | Just an alias entry in profiles |
| `custom_openai_compatible`     | Generic OpenAI-compatible                      | For self-hosted / 3rd-party providers |

Adding a new protocol = adding a new `Adapter` subclass and an entry in
`ProviderRegistry`. The legacy code already had this shape; v2 inherits.

### Capabilities

A `Capabilities` bitfield per route lets the agent code branch on what's available
without testing provider names:

```cpp
struct Capabilities {
  bool streaming           = false;
  bool tool_use            = false;
  bool extended_thinking   = false;
  bool vision_image        = false;
  bool audio_in            = false;
  bool prompt_cache        = false;
  bool json_mode           = false;
  bool parallel_tool_calls = false;
  std::uint32_t max_context_tokens = 0;
};
```

Code asks `route.primary.caps.tool_use`, not `route.is_anthropic()`. Vendor lock-in is
avoided by construction.

### Execution Layer

Concerns owned by `execution::Runtime`:

- **Retry**: configurable per-route. Idempotent retry for transport errors, no retry
  for upstream-classified semantic errors.
- **Fallback**: on retryable failure of the primary, try fallbacks in order.
- **Usage aggregation**: per-agent, per-route, per-day counters in `audit.db`.
- **Cost tracking**: profiles declare cost/1M tokens; aggregator computes spend and
  emits `provider.cost_threshold` hooks.
- **Hooks**: `provider_request` / `provider_response` / `provider_error` /
  `provider_fallback` (see `permissions-and-hooks.md`).

## Streaming

All adapters support streaming when the vendor supports it. The contract:

- The `Awaitable<Result<Response>>` resolves only when the stream terminates.
- During streaming, the `EventSink` (provided by the caller) receives deltas.
- The web UI's SSE route bridges these deltas to the client.
- The CLI REPL renders deltas to the terminal in real time.

## Caching

Prompt caching is **first-class**. The agent's prompt builder produces a list of
`CacheSection`s, each with a cache key. Adapters map these to vendor cache APIs:

- Anthropic: `cache_control: { type: "ephemeral" }` blocks.
- OpenAI Responses: prompt prefix hashing where available.
- Others: silently ignored.

### Cache Key Versioning

Each section carries a `cache_version` integer. When upstream caching is
provider-managed (Anthropic), the key is opaque; when we need to invalidate (e.g., a
skill activated/deactivated), we bump the version. The legacy code's string-concat
cache key is replaced by a tuple `(section_id, content_hash, version)`.

## Configuration Shape

```jsonc
{
  "profiles": {
    "anthropic-main": {
      "base_url": "https://api.anthropic.com",
      "api_key":  "${ANTHROPIC_API_KEY}",
      "headers":  { "anthropic-version": "2023-06-01" },
      "models": {
        "claude-opus-4-7": {
          "protocol": "anthropic_messages",
          "context_window": 200000,
          "thinking": "medium",
          "cost_per_1m_input":  15.0,
          "cost_per_1m_output": 75.0
        }
      }
    },
    "openai-main": {
      "base_url": "https://api.openai.com",
      "api_key":  "${OPENAI_API_KEY}",
      "models": {
        "gpt-5-thinking": {
          "protocol": "openai_responses",
          "context_window": 256000
        }
      }
    }
  },
  "routes": {
    "default":   { "primary": "anthropic-main:claude-opus-4-7", "fallbacks": ["openai-main:gpt-5-thinking"] },
    "coder":     { "primary": "anthropic-main:claude-opus-4-7" },
    "research":  { "primary": "openai-main:gpt-5-thinking" }
  }
}
```

## Error Categories

```cpp
enum class ErrorCategory {
  config,          // misconfiguration — not retryable
  auth,            // bad credentials — not retryable
  network,         // transport error — retryable
  rate_limit,      // upstream throttle — retryable with backoff
  upstream,        // vendor 5xx — retryable
  parsing,         // we got something we didn't expect — log + escalate
  invalid_request, // we sent something the vendor rejected — not retryable
  interrupted,     // cancelled
  unknown,
};
```

`Error::retryable()` is the canonical predicate; execution layer uses it.

## Anti-Patterns

- Reaching into protocol-specific JSON in agent code. Always go via `Response::blocks`.
- Per-adapter retry logic. Retry is execution-layer.
- Mixing async styles. Adapters return `Awaitable<Result<Response>>`, period.
- Hardcoding model names. Use config profiles and routes.

## Bench

`bench/oran-provider/` ships:

- `bench/request-encode` — serializing a 32-message conversation to vendor JSON.
- `bench/response-decode` — parsing a streamed response (synthetic; no network).
- `bench/protocol-overhead` — A/B between adapters on the same canonical request.

## See Also

- [`async-model.md`](async-model.md) — how streams interact with the executor.
- [`../product-specs/0001-core-react-loop.md`](../product-specs/0001-core-react-loop.md)
  — first concrete v1 deliverable.
- Legacy `src/providers/` for reference (not copy).
