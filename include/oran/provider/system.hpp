#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/cache.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider {

/// Wire-format family for a vendor endpoint. The execution layer dispatches on
/// this enumerator (not on the provider name) so adding a new endpoint that
/// reuses an existing protocol is a config change, not code.
///
/// Wire spelling comes from `core::enum_name(value)` — no hand-maintained
/// string table per `docs/rules/code-style.md`.
enum class ProtocolKind : std::uint8_t {
  anthropic_messages,
  openai_chat_completions,
  openai_responses,
  gemini_generate_content,
  custom_openai_compatible,
};

/// USD pricing per one million provider tokens. Input and output prices are
/// independent; cache token prices fall back to input pricing when unset so a
/// partially specified profile still produces a conservative rollup.
struct ProviderPricing {
  std::optional<double> input_per_million_usd{};
  std::optional<double> output_per_million_usd{};
  std::optional<double> cache_creation_per_million_usd{};
  std::optional<double> cache_read_per_million_usd{};

  [[nodiscard]] bool empty() const noexcept {
    return !input_per_million_usd.has_value() && !output_per_million_usd.has_value() &&
           !cache_creation_per_million_usd.has_value() && !cache_read_per_million_usd.has_value();
  }

  friend bool operator==(const ProviderPricing&, const ProviderPricing&) = default;
};

/// One configured model and its per-attempt policy. Profiles select endpoint
/// credentials; protocol determines wire conversion.
struct ModelTarget {
  std::string profile;
  std::string model;
  ProtocolKind protocol{ProtocolKind::anthropic_messages};
  std::optional<std::uint32_t> thinking_budget;
  std::optional<PromptCacheOptions> cache;
  ProviderPricing pricing{};

  friend bool operator==(const ModelTarget&, const ModelTarget&) = default;
};

/// Resolved primary and fallback chain, reused across a session's turns.
struct Route {
  ModelTarget primary;
  std::vector<ModelTarget> fallbacks;

  friend bool operator==(const Route&, const Route&) = default;
};

/// Streaming observer. Adapters call into the sink as deltas arrive on the
/// wire; the sink converts them into whatever surface the caller cares about
/// (text rendering, SSE forwarding, or batch consumption).
///
/// All methods default to no-ops so callers override only the deltas they
/// care about. The contract: deltas for one block arrive in order; blocks may
/// be interleaved by `id`; `on_done` is the last call and carries the final
/// `StopReason` that will appear in the returned `Response`.
///
/// Concurrency. Sinks are not thread-safe; the provider drives them from
/// whichever coroutine ran `send`. Sinks that need to dispatch elsewhere are
/// responsible for hopping onto their own strand.
class EventSink {
public:
  EventSink() = default;
  virtual ~EventSink() = default;

  EventSink(const EventSink&) = delete;
  EventSink& operator=(const EventSink&) = delete;
  EventSink(EventSink&&) = delete;
  EventSink& operator=(EventSink&&) = delete;

  /// A delta of free-form assistant text.
  virtual void on_text_delta(std::string_view delta) {
    static_cast<void>(delta);
  }

  /// A delta of extended-thinking text (Anthropic) / reasoning trace.
  virtual void on_thinking_delta(std::string_view delta) {
    static_cast<void>(delta);
  }

  /// The provider opened a `tool_use` block. `id` is the vendor-issued unique
  /// id; `name` is the tool name.
  virtual void on_tool_start(std::string_view id, std::string_view name) {
    static_cast<void>(id);
    static_cast<void>(name);
  }

  /// A partial chunk of the JSON input for the tool block identified by `id`.
  virtual void on_tool_delta(std::string_view id, std::string_view input_delta) {
    static_cast<void>(id);
    static_cast<void>(input_delta);
  }

  /// Terminal callback. `stop_reason` matches the `Response::stop_reason` that
  /// `send` will return.
  virtual void on_done(core::StopReason stop_reason) {
    static_cast<void>(stop_reason);
  }
};

/// Entry point for one provider attempt. `make_protocol_system` composes protocol
/// mapping with an injected transport; `FakeProvider` supplies controlled turns.
///
/// The method is `const` by design: providers may be shared across concurrent
/// turns from different agents on the same process. Any internal mutable
/// state (retry counters, scripted-turn cursors) lives behind synchronisation
/// in the concrete subclass.
class System {
public:
  System() = default;
  virtual ~System() = default;

  System(const System&) = delete;
  System& operator=(const System&) = delete;
  System(System&&) = delete;
  System& operator=(System&&) = delete;

  /// Return an assembled response or a classified provider error.
  ///
  /// `sink` is optional: if non-null the provider calls into it with stream
  /// deltas before returning. If null the provider still synthesises and
  /// returns the same `Response`. The sink is borrowed for the duration of
  /// the call only.
  [[nodiscard]] virtual async::Awaitable<core::Result<Response>>
  send(Request request, ModelTarget target, EventSink* sink = nullptr) const = 0;
};

}  // namespace orangutan::provider
