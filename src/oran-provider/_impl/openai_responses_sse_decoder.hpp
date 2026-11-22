// src/oran-provider/_impl/openai_responses_sse_decoder.hpp - incremental OpenAI Responses SSE decoder.
//
// Stateful sibling of `decode_protocol_response`: it consumes one OpenAI
// Responses `text/event-stream` event at a time, drives an `EventSink` with
// ordered deltas, and uses the terminal response event as the authoritative
// assembled `provider::Response`.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider::detail {

/// Incremental decoder for the OpenAI Responses SSE event stream.
///
/// Feed each decoded SSE event through `consume`. Text/reasoning/function-call
/// argument deltas are forwarded to the optional `EventSink` as they arrive.
/// The final `response.completed`, `response.incomplete`, or `response.failed`
/// event carries the full response object; `result()` decodes that object
/// through the existing body decoder so the streaming and body paths assemble
/// byte-identical domain responses for supported output shapes.
class OpenAiResponsesSseDecoder {
public:
  explicit OpenAiResponsesSseDecoder(ModelTarget target, EventSink* sink = nullptr);

  /// Feed one decoded SSE event. Once an error has been recorded every later
  /// event is ignored so the first failure wins. Never throws.
  void consume(std::string_view event, std::string_view data);

  /// The first decode error, or the assembled `Response` once the stream
  /// completed. An error is returned for `error` events, malformed payloads, or
  /// streams that end before a terminal response event.
  [[nodiscard]] core::Result<Response> result() const;

private:
  struct FunctionCallState {
    std::string item_id;
    std::string call_id;
    std::string name;
    std::string arguments;
    bool started{false};
  };

  void fail(core::Error error);
  void maybe_start_tool(FunctionCallState& state);

  ModelTarget target_;
  EventSink* sink_;
  std::optional<core::Error> error_;
  std::optional<std::string> final_response_json_;
  std::unordered_map<std::string, FunctionCallState> function_calls_;
};

}  // namespace orangutan::provider::detail
