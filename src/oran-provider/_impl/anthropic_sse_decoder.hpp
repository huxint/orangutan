// src/oran-provider/_impl/anthropic_sse_decoder.hpp - incremental Anthropic Messages SSE decoder.
//
// Stateful sibling of `decode_protocol_response`: it consumes one Anthropic
// `text/event-stream` event at a time, drives an `EventSink` with ordered
// deltas, and assembles the same `provider::Response` the body decoder would
// produce for the equivalent non-streaming body. This is an internal detail of
// the streaming `ProtocolTransportSystem`, so it lives under `_impl/` and keeps
// `nlohmann` in the `.cpp`.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider::detail {

/// Incremental decoder for the Anthropic Messages SSE event stream.
///
/// Feed each decoded SSE event (the `event:` name plus the joined `data:`
/// payload) through `consume`. The decoder advances a strict state machine
/// (`message_start` → `content_block_start`/`_delta`/`_stop` … → `message_delta`
/// → `message_stop`), updates block/usage state, and calls the optional
/// `EventSink` with ordered deltas as they arrive. `message_stop` finalises the
/// assembled `Response` and calls `sink->on_done`. `ping` events are ignored; an
/// `error` event or a malformed payload records the first decode error.
///
/// The callback type that drives this decoder is `void(std::string_view,
/// std::string_view)`, so errors cannot flow back through `consume`; they are
/// stored and surfaced by `result()` after the stream resolves.
class AnthropicSseDecoder {
public:
  explicit AnthropicSseDecoder(ModelTarget target, EventSink* sink = nullptr);

  /// Feed one decoded SSE event. Once an error has been recorded every later
  /// event is ignored so the first failure wins. Never throws.
  void consume(std::string_view event, std::string_view data);

  /// The first decode error, or the assembled `Response` once the stream
  /// completed. An error is returned for a malformed/unsupported event, an
  /// `error` event, or a stream that ended before `message_stop`.
  [[nodiscard]] core::Result<Response> result() const;

private:
  struct OpenBlock {
    enum class Kind : std::uint8_t {
      none,
      text,
      thinking,
      tool_use
    };
    Kind kind{Kind::none};
    std::string text;
    std::optional<std::string> signature;
    std::string tool_id;
    std::string tool_name;
    std::string tool_input;
  };

  /// Record the first decode error; later calls are no-ops so the first failure
  /// wins. JSON parsing happens in the `.cpp`, so the dispatch lives there too.
  void fail(core::Error error);

  /// Push the currently open block onto `blocks_` (parsing accumulated tool
  /// input JSON) and reset the open-block accumulator.
  void finalize_open_block();

  ModelTarget target_;
  EventSink* sink_;
  std::vector<core::Content> blocks_;
  OpenBlock open_;
  Usage usage_{};
  core::StopReason stop_reason_{core::StopReason::end_turn};
  std::optional<std::string> model_;
  std::optional<core::Error> error_;
  bool saw_message_stop_{false};
};

}  // namespace orangutan::provider::detail
