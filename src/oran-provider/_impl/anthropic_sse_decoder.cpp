// src/oran-provider/_impl/anthropic_sse_decoder.cpp - incremental Anthropic Messages SSE decoder.

#include "anthropic_sse_decoder.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::provider::detail {
namespace {

using json = ::nlohmann::ordered_json;
using orangutan::core::Error;

[[nodiscard]] std::string protocol_name(const ModelTarget& target) {
  return std::string{core::enum_name(target.protocol)};
}

[[nodiscard]] core::StopReason anthropic_stop_reason(std::string_view reason) noexcept {
  return core::parse_enum<core::StopReason>(reason).value_or(core::StopReason::error);
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

[[nodiscard]] std::uint64_t u64_or_zero(const json& object, std::string_view key) noexcept {
  const auto* value = member(object, key);
  if (value == nullptr) {
    return 0;
  }
  if (value->is_number_unsigned()) {
    return value->get<std::uint64_t>();
  }
  if (value->is_number_integer()) {
    const auto signed_value = value->get<std::int64_t>();
    return signed_value >= 0 ? static_cast<std::uint64_t>(signed_value) : 0;
  }
  return 0;
}

}  // namespace

AnthropicSseDecoder::AnthropicSseDecoder(ModelTarget target, EventSink* sink)
    : target_{std::move(target)}, sink_{sink} {}

void AnthropicSseDecoder::consume(std::string_view event, std::string_view data) {
  if (error_.has_value()) {
    return;  // The first failure wins; ignore the rest of the stream.
  }
  if (event == "ping") {
    return;
  }

  json payload;
  try {
    payload = json::parse(data.begin(), data.end());
  } catch (const json::parse_error& error) {
    fail(Error::parsing("anthropic stream event is not valid JSON")
             .with("protocol", protocol_name(target_))
             .with("event", std::string{event})
             .with("detail", error.what()));
    return;
  }
  if (!payload.is_object()) {
    fail(Error::parsing("anthropic stream event must be a JSON object")
             .with("protocol", protocol_name(target_))
             .with("event", std::string{event}));
    return;
  }

  if (event == "message_start") {
    const auto* message = member(payload, "message");
    if (message == nullptr || !message->is_object()) {
      return;
    }
    if (const auto model = string_or_empty(*message, "model"); !model.empty()) {
      model_ = std::string{model};
    }
    if (const auto* usage = member(*message, "usage"); usage != nullptr && usage->is_object()) {
      usage_.input_tokens = u64_or_zero(*usage, "input_tokens");
      usage_.output_tokens = u64_or_zero(*usage, "output_tokens");
      usage_.cache_creation_tokens = u64_or_zero(*usage, "cache_creation_input_tokens");
      usage_.cache_read_tokens = u64_or_zero(*usage, "cache_read_input_tokens");
    }
    return;
  }

  if (event == "content_block_start") {
    if (open_.kind != OpenBlock::Kind::none) {
      fail(Error::parsing("anthropic content block started before the previous block stopped")
               .with("protocol", protocol_name(target_)));
      return;
    }
    const auto* block = member(payload, "content_block");
    if (block == nullptr || !block->is_object()) {
      fail(Error::parsing("anthropic content_block_start is missing content_block")
               .with("protocol", protocol_name(target_)));
      return;
    }
    open_ = OpenBlock{};
    const auto type = string_or_empty(*block, "type");
    if (type == "text") {
      open_.kind = OpenBlock::Kind::text;
    } else if (type == "thinking") {
      open_.kind = OpenBlock::Kind::thinking;
    } else if (type == "tool_use") {
      open_.kind = OpenBlock::Kind::tool_use;
      open_.tool_id = std::string{string_or_empty(*block, "id")};
      open_.tool_name = std::string{string_or_empty(*block, "name")};
      if (sink_ != nullptr) {
        sink_->on_tool_start(open_.tool_id, open_.tool_name);
      }
    } else {
      fail(Error::parsing("anthropic content block type is not supported")
               .with("protocol", protocol_name(target_))
               .with("type", std::string{type}));
    }
    return;
  }

  if (event == "content_block_delta") {
    if (open_.kind == OpenBlock::Kind::none) {
      fail(Error::parsing("anthropic content block delta has no open block").with("protocol", protocol_name(target_)));
      return;
    }
    const auto* delta = member(payload, "delta");
    if (delta == nullptr || !delta->is_object()) {
      fail(Error::parsing("anthropic content_block_delta is missing delta").with("protocol", protocol_name(target_)));
      return;
    }
    const auto type = string_or_empty(*delta, "type");
    if (type == "text_delta") {
      const auto text = string_or_empty(*delta, "text");
      open_.text.append(text);
      if (sink_ != nullptr) {
        if (open_.kind == OpenBlock::Kind::thinking) {
          sink_->on_thinking_delta(text);
        } else {
          sink_->on_text_delta(text);
        }
      }
    } else if (type == "thinking_delta") {
      const auto thinking = string_or_empty(*delta, "thinking");
      open_.text.append(thinking);
      if (sink_ != nullptr) {
        sink_->on_thinking_delta(thinking);
      }
    } else if (type == "signature_delta") {
      open_.signature = open_.signature.value_or(std::string{}) + std::string{string_or_empty(*delta, "signature")};
    } else if (type == "input_json_delta") {
      const auto partial = string_or_empty(*delta, "partial_json");
      open_.tool_input.append(partial);
      if (sink_ != nullptr) {
        sink_->on_tool_delta(open_.tool_id, partial);
      }
    }
    // Unknown delta subtypes are advisory and ignored for forward compatibility.
    return;
  }

  if (event == "content_block_stop") {
    if (open_.kind == OpenBlock::Kind::none) {
      fail(Error::parsing("anthropic content block stop has no open block").with("protocol", protocol_name(target_)));
      return;
    }
    finalize_open_block();
    return;
  }

  if (event == "message_delta") {
    if (const auto* delta = member(payload, "delta"); delta != nullptr && delta->is_object()) {
      if (const auto reason = string_or_empty(*delta, "stop_reason"); !reason.empty()) {
        stop_reason_ = anthropic_stop_reason(reason);
      }
    }
    if (const auto* usage = member(payload, "usage"); usage != nullptr && usage->is_object()) {
      usage_.output_tokens = u64_or_zero(*usage, "output_tokens");
    }
    return;
  }

  if (event == "message_stop") {
    if (open_.kind != OpenBlock::Kind::none) {
      fail(Error::parsing("anthropic message stopped before the content block completed")
               .with("protocol", protocol_name(target_)));
      return;
    }
    saw_message_stop_ = true;
    if (sink_ != nullptr) {
      sink_->on_done(stop_reason_);
    }
    return;
  }

  if (event == "error") {
    const auto* error = member(payload, "error");
    const auto error_type = error != nullptr ? string_or_empty(*error, "type") : std::string_view{};
    const auto message = error != nullptr ? string_or_empty(*error, "message") : std::string_view{};
    fail(Error::upstream("anthropic stream returned an error event")
             .with("protocol", protocol_name(target_))
             .with("error_type", std::string{error_type})
             .with("error_message", std::string{message}));
    return;
  }

  // Unknown event names are ignored so additive vendor events do not break the
  // stream (mirrors the SSE grammar's "ignore unknown field" stance).
}

void AnthropicSseDecoder::finalize_open_block() {
  switch (open_.kind) {
    case OpenBlock::Kind::text:
      blocks_.emplace_back(core::TextContent{.text = std::move(open_.text)});
      break;
    case OpenBlock::Kind::thinking:
      blocks_.emplace_back(core::ThinkingContent{
          .thinking = std::move(open_.text),
          .signature = std::move(open_.signature),
      });
      break;
    case OpenBlock::Kind::tool_use: {
      const auto input_text = open_.tool_input.empty() ? std::string{"{}"} : open_.tool_input;
      json input;
      try {
        input = json::parse(input_text);
      } catch (const json::parse_error& error) {
        fail(Error::parsing("anthropic tool input json is malformed")
                 .with("protocol", protocol_name(target_))
                 .with("tool_use_id", open_.tool_id)
                 .with("tool", open_.tool_name)
                 .with("detail", error.what()));
        return;
      }
      if (!input.is_object()) {
        fail(Error::parsing("anthropic tool input must be a JSON object")
                 .with("protocol", protocol_name(target_))
                 .with("tool_use_id", open_.tool_id)
                 .with("tool", open_.tool_name));
        return;
      }
      blocks_.emplace_back(core::ToolUseContent{
          .id = std::move(open_.tool_id),
          .name = std::move(open_.tool_name),
          .input_json = input.dump(),
      });
      break;
    }
    case OpenBlock::Kind::none:
      break;  // A stop without a matching start is ignored defensively.
  }
  open_ = OpenBlock{};
}

void AnthropicSseDecoder::fail(core::Error error) {
  if (!error_.has_value()) {
    error_ = std::move(error);
  }
}

core::Result<Response> AnthropicSseDecoder::result() const {
  if (error_.has_value()) {
    return std::unexpected(*error_);
  }
  if (!saw_message_stop_) {
    return std::unexpected(
        Error::parsing("anthropic stream ended before message_stop").with("protocol", protocol_name(target_)));
  }
  return Response{
      .blocks = blocks_,
      .stop_reason = stop_reason_,
      .usage = usage_,
      .model_used = model_,
  };
}

}  // namespace orangutan::provider::detail
