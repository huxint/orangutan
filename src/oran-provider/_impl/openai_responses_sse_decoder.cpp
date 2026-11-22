// src/oran-provider/_impl/openai_responses_sse_decoder.cpp - incremental OpenAI Responses SSE decoder.

#include "openai_responses_sse_decoder.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/provider/protocol_response.hpp>

namespace orangutan::provider::detail {
namespace {

using json = ::nlohmann::ordered_json;
using orangutan::core::Error;

[[nodiscard]] std::string protocol_name(const ModelTarget& target) {
  return std::string{core::enum_name(target.protocol)};
}

[[nodiscard]] const json* member(const json& object, std::string_view key) noexcept {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

[[nodiscard]] std::string_view string_or_empty(const json& object, std::string_view key) noexcept {
  const auto* value = member(object, key);
  return value != nullptr && value->is_string() ? value->get<std::string_view>() : std::string_view{};
}

[[nodiscard]] const json* object_member(const json& object, std::string_view key) noexcept {
  const auto* value = member(object, key);
  return value != nullptr && value->is_object() ? value : nullptr;
}

[[nodiscard]] core::Error failed_response_error(const json& response, const ModelTarget& target) {
  const auto* error = object_member(response, "error");
  const auto code = error != nullptr ? string_or_empty(*error, "code") : std::string_view{};
  const auto message = error != nullptr ? string_or_empty(*error, "message") : std::string_view{};

  auto classified = code == "rate_limit_exceeded" ? Error::rate_limit("openai responses stream failed")
                                                  : Error::upstream("openai responses stream failed");
  return std::move(classified)
      .with("protocol", protocol_name(target))
      .with("error_code", std::string{code})
      .with("error_message", std::string{message});
}

}  // namespace

OpenAiResponsesSseDecoder::OpenAiResponsesSseDecoder(ModelTarget target, EventSink* sink)
    : target_{std::move(target)}, sink_{sink} {}

void OpenAiResponsesSseDecoder::consume(std::string_view event, std::string_view data) {
  if (error_.has_value()) {
    return;
  }

  json payload;
  try {
    payload = json::parse(data.begin(), data.end());
  } catch (const json::parse_error& error) {
    fail(Error::parsing("openai responses stream event is not valid JSON")
             .with("protocol", protocol_name(target_))
             .with("event", std::string{event})
             .with("detail", error.what()));
    return;
  }
  if (!payload.is_object()) {
    fail(Error::parsing("openai responses stream event must be a JSON object")
             .with("protocol", protocol_name(target_))
             .with("event", std::string{event}));
    return;
  }

  if (event == "response.output_item.added") {
    const auto* item = object_member(payload, "item");
    if (item == nullptr) {
      return;
    }
    if (string_or_empty(*item, "type") != "function_call") {
      return;
    }
    const auto item_id = string_or_empty(*item, "id");
    if (item_id.empty()) {
      return;
    }
    auto& state = function_calls_[std::string{item_id}];
    state.item_id = std::string{item_id};
    state.call_id = std::string{string_or_empty(*item, "call_id")};
    state.name = std::string{string_or_empty(*item, "name")};
    if (const auto arguments = string_or_empty(*item, "arguments"); !arguments.empty()) {
      state.arguments = std::string{arguments};
    }
    maybe_start_tool(state);
    return;
  }

  if (event == "response.output_text.delta") {
    if (sink_ != nullptr) {
      sink_->on_text_delta(string_or_empty(payload, "delta"));
    }
    return;
  }

  if (event == "response.reasoning_summary_text.delta" || event == "response.reasoning_text.delta") {
    if (sink_ != nullptr) {
      sink_->on_thinking_delta(string_or_empty(payload, "delta"));
    }
    return;
  }

  if (event == "response.function_call_arguments.delta") {
    const auto item_id = string_or_empty(payload, "item_id");
    const auto delta = string_or_empty(payload, "delta");
    if (!item_id.empty()) {
      auto& state = function_calls_[std::string{item_id}];
      state.item_id = std::string{item_id};
      state.arguments.append(delta);
      if (sink_ != nullptr) {
        sink_->on_tool_delta(state.call_id.empty() ? state.item_id : state.call_id, delta);
      }
    }
    return;
  }

  if (event == "response.function_call_arguments.done") {
    const auto item_id = string_or_empty(payload, "item_id");
    if (!item_id.empty()) {
      auto& state = function_calls_[std::string{item_id}];
      state.item_id = std::string{item_id};
      state.name = std::string{string_or_empty(payload, "name")};
      state.arguments = std::string{string_or_empty(payload, "arguments")};
      maybe_start_tool(state);
    }
    return;
  }

  if (event == "response.completed" || event == "response.incomplete" || event == "response.failed") {
    const auto* response = object_member(payload, "response");
    if (response == nullptr) {
      fail(Error::parsing("openai responses terminal stream event is missing response")
               .with("protocol", protocol_name(target_))
               .with("event", std::string{event}));
      return;
    }
    if (event == "response.failed") {
      fail(failed_response_error(*response, target_));
      return;
    }
    final_response_json_ = response->dump();
    return;
  }

  if (event == "error") {
    fail(Error::upstream("openai responses stream returned an error event")
             .with("protocol", protocol_name(target_))
             .with("error_code", std::string{string_or_empty(payload, "code")})
             .with("error_message", std::string{string_or_empty(payload, "message")}));
    return;
  }

  // Unknown or currently unconsumed events (created/in_progress/content_part,
  // output_item.done, annotations, built-in tool progress, obfuscation) are
  // ignored; the terminal response event remains the authoritative state.
}

void OpenAiResponsesSseDecoder::fail(core::Error error) {
  if (!error_.has_value()) {
    error_ = std::move(error);
  }
}

void OpenAiResponsesSseDecoder::maybe_start_tool(FunctionCallState& state) {
  if (sink_ == nullptr || state.name.empty()) {
    return;
  }
  if (state.started) {
    return;
  }
  state.started = true;
  const auto id = !state.call_id.empty() ? std::string_view{state.call_id} : std::string_view{state.item_id};
  sink_->on_tool_start(id, state.name);
}

core::Result<Response> OpenAiResponsesSseDecoder::result() const {
  if (error_.has_value()) {
    return std::unexpected(*error_);
  }
  if (!final_response_json_.has_value()) {
    return std::unexpected(Error::parsing("openai responses stream ended before terminal response event")
                               .with("protocol", protocol_name(target_)));
  }
  auto decoded = decode_protocol_response(*final_response_json_, target_);
  if (!decoded) {
    return std::unexpected(std::move(decoded).error());
  }
  if (sink_ != nullptr) {
    sink_->on_done(decoded->stop_reason);
  }
  return decoded;
}

}  // namespace orangutan::provider::detail
