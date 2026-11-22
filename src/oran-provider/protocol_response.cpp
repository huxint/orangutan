// src/oran-provider/protocol_response.cpp - provider protocol response bytes.

#include <oran/provider/protocol_response.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/content.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>

namespace orangutan::provider {
namespace {

using json = ::nlohmann::ordered_json;
using orangutan::core::Error;

[[nodiscard]] std::string protocol_name(ProtocolKind protocol) {
  return std::string{core::enum_name(protocol)};
}

[[nodiscard]] Error protocol_error(std::string message, const ModelTarget& target) {
  return Error::config(std::move(message))
      .with("profile", target.profile)
      .with("model", target.model)
      .with("protocol", protocol_name(target.protocol));
}

[[nodiscard]] Error parse_error(std::string message, std::string field, const ModelTarget& target) {
  return Error::parsing(std::move(message))
      .with("field", std::move(field))
      .with("protocol", protocol_name(target.protocol));
}

[[nodiscard]] core::Result<json>
parse_json_object(std::string_view text, std::string field, const ModelTarget& target) {
  try {
    auto parsed = json::parse(text.begin(), text.end());
    if (!parsed.is_object()) {
      return std::unexpected(
          parse_error("provider protocol response field must be a JSON object", std::move(field), target));
    }
    return parsed;
  } catch (const json::parse_error& error) {
    return std::unexpected(parse_error("provider protocol response field is not valid JSON", std::move(field), target)
                               .with("detail", error.what()));
  } catch (const json::type_error& error) {
    return std::unexpected(parse_error("provider protocol response field is not valid JSON", std::move(field), target)
                               .with("detail", error.what()));
  }
}

[[nodiscard]] core::Result<json> parse_body_json(std::string_view text, const ModelTarget& target) {
  auto body = parse_json_object(text, "body", target);
  if (!body) {
    return std::unexpected(std::move(body).error().with("scope", "response"));
  }
  return body;
}

[[nodiscard]] const json* member(const json& object, std::string_view key) noexcept {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

[[nodiscard]] core::Result<std::string>
required_string(const json& object, std::string_view key, std::string field, const ModelTarget& target) {
  const auto* value = member(object, key);
  if (value == nullptr || !value->is_string()) {
    return std::unexpected(parse_error("provider protocol response field must be a string", std::move(field), target));
  }
  return value->get<std::string>();
}

[[nodiscard]] core::Result<std::string>
optional_string(const json& object, std::string_view key, std::string field, const ModelTarget& target) {
  const auto* value = member(object, key);
  if (value == nullptr || value->is_null()) {
    return std::string{};
  }
  if (!value->is_string()) {
    return std::unexpected(parse_error("provider protocol response field must be a string", std::move(field), target));
  }
  return value->get<std::string>();
}

[[nodiscard]] core::Result<std::uint64_t>
unsigned_integer_value(const json& value, std::string field, const ModelTarget& target) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value >= 0) {
      return static_cast<std::uint64_t>(signed_value);
    }
  }
  return std::unexpected(
      parse_error("provider protocol response field must be a non-negative integer", std::move(field), target));
}

[[nodiscard]] core::Result<std::uint64_t>
optional_u64(const json& object, std::string_view key, std::string field, const ModelTarget& target) {
  const auto* value = member(object, key);
  if (value == nullptr || value->is_null()) {
    return 0U;
  }
  return unsigned_integer_value(*value, std::move(field), target);
}

[[nodiscard]] core::Result<json>
required_array(const json& object, std::string_view key, std::string field, const ModelTarget& target) {
  const auto* value = member(object, key);
  if (value == nullptr || !value->is_array()) {
    return std::unexpected(parse_error("provider protocol response field must be an array", std::move(field), target));
  }
  return *value;
}

[[nodiscard]] core::StopReason anthropic_stop_reason(std::string_view reason) noexcept {
  return core::parse_enum<core::StopReason>(reason).value_or(core::StopReason::error);
}

[[nodiscard]] core::Result<core::Content>
anthropic_content_block(const json& block, std::size_t index, const ModelTarget& target) {
  if (!block.is_object()) {
    return std::unexpected(
        parse_error("anthropic content block must be an object", "content[" + std::to_string(index) + "]", target));
  }

  auto type = required_string(block, "type", "content[" + std::to_string(index) + "].type", target);
  if (!type) {
    return std::unexpected(std::move(type).error());
  }

  if (*type == "text") {
    auto text = required_string(block, "text", "content[" + std::to_string(index) + "].text", target);
    if (!text) {
      return std::unexpected(std::move(text).error());
    }
    return core::Content{core::TextContent{.text = std::move(*text)}};
  }

  if (*type == "thinking") {
    auto thinking = required_string(block, "thinking", "content[" + std::to_string(index) + "].thinking", target);
    if (!thinking) {
      return std::unexpected(std::move(thinking).error());
    }
    auto signature = optional_string(block, "signature", "content[" + std::to_string(index) + "].signature", target);
    if (!signature) {
      return std::unexpected(std::move(signature).error());
    }
    return core::Content{core::ThinkingContent{
        .thinking = std::move(*thinking),
        .signature = signature->empty() ? std::nullopt : std::optional<std::string>{std::move(*signature)},
    }};
  }

  if (*type == "tool_use") {
    auto id = required_string(block, "id", "content[" + std::to_string(index) + "].id", target);
    if (!id) {
      return std::unexpected(std::move(id).error());
    }
    auto name = required_string(block, "name", "content[" + std::to_string(index) + "].name", target);
    if (!name) {
      return std::unexpected(std::move(name).error());
    }
    const auto* input = member(block, "input");
    if (input == nullptr || !input->is_object()) {
      return std::unexpected(parse_error("anthropic tool input must be a JSON object",
                                         "content[" + std::to_string(index) + "].input",
                                         target)
                                 .with("tool_use_id", *id)
                                 .with("tool", *name));
    }
    return core::Content{
        core::ToolUseContent{.id = std::move(*id), .name = std::move(*name), .input_json = input->dump()}};
  }

  return std::unexpected(parse_error("anthropic content block type is not supported",
                                     "content[" + std::to_string(index) + "].type",
                                     target)
                             .with("type", std::move(*type)));
}

[[nodiscard]] core::Result<Usage> anthropic_usage(const json& body, const ModelTarget& target) {
  const auto* usage = member(body, "usage");
  if (usage == nullptr || usage->is_null()) {
    return Usage{};
  }
  if (!usage->is_object()) {
    return std::unexpected(parse_error("anthropic usage field must be an object", "usage", target));
  }

  auto input = optional_u64(*usage, "input_tokens", "usage.input_tokens", target);
  if (!input) {
    return std::unexpected(std::move(input).error());
  }
  auto output = optional_u64(*usage, "output_tokens", "usage.output_tokens", target);
  if (!output) {
    return std::unexpected(std::move(output).error());
  }
  auto cache_creation =
      optional_u64(*usage, "cache_creation_input_tokens", "usage.cache_creation_input_tokens", target);
  if (!cache_creation) {
    return std::unexpected(std::move(cache_creation).error());
  }
  auto cache_read = optional_u64(*usage, "cache_read_input_tokens", "usage.cache_read_input_tokens", target);
  if (!cache_read) {
    return std::unexpected(std::move(cache_read).error());
  }

  return Usage{
      .input_tokens = *input,
      .output_tokens = *output,
      .cache_creation_tokens = *cache_creation,
      .cache_read_tokens = *cache_read,
      .cost_estimate = std::nullopt,
  };
}

[[nodiscard]] core::Result<Response> decode_anthropic_response(std::string_view body_json, const ModelTarget& target) {
  auto body = parse_body_json(body_json, target);
  if (!body) {
    return std::unexpected(std::move(body).error());
  }

  auto content = required_array(*body, "content", "content", target);
  if (!content) {
    return std::unexpected(std::move(content).error());
  }

  std::vector<core::Content> blocks;
  blocks.reserve(content->size());
  for (std::size_t index = 0; index < content->size(); ++index) {
    auto decoded = anthropic_content_block((*content)[index], index, target);
    if (!decoded) {
      return std::unexpected(std::move(decoded).error());
    }
    blocks.push_back(std::move(*decoded));
  }

  auto stop_reason = required_string(*body, "stop_reason", "stop_reason", target);
  if (!stop_reason) {
    return std::unexpected(std::move(stop_reason).error());
  }
  auto usage = anthropic_usage(*body, target);
  if (!usage) {
    return std::unexpected(std::move(usage).error());
  }
  auto model = optional_string(*body, "model", "model", target);
  if (!model) {
    return std::unexpected(std::move(model).error());
  }

  return Response{
      .blocks = std::move(blocks),
      .stop_reason = anthropic_stop_reason(*stop_reason),
      .usage = *usage,
      .model_used = model->empty() ? std::nullopt : std::optional<std::string>{std::move(*model)},
  };
}

[[nodiscard]] core::Result<void> append_openai_message_content(const json& item,
                                                               std::size_t item_index,
                                                               std::vector<core::Content>& blocks,
                                                               const ModelTarget& target) {
  auto role = optional_string(item, "role", "output[" + std::to_string(item_index) + "].role", target);
  if (!role) {
    return std::unexpected(std::move(role).error());
  }
  if (!role->empty() && *role != "assistant") {
    return std::unexpected(parse_error("openai response message role is not supported",
                                       "output[" + std::to_string(item_index) + "].role",
                                       target)
                               .with("role", std::move(*role)));
  }

  auto content = required_array(item, "content", "output[" + std::to_string(item_index) + "].content", target);
  if (!content) {
    return std::unexpected(std::move(content).error());
  }
  for (std::size_t content_index = 0; content_index < content->size(); ++content_index) {
    const auto& block = (*content)[content_index];
    if (!block.is_object()) {
      return std::unexpected(
          parse_error("openai message content block must be an object",
                      "output[" + std::to_string(item_index) + "].content[" + std::to_string(content_index) + "]",
                      target));
    }
    auto type = required_string(block,
                                "type",
                                "output[" + std::to_string(item_index) + "].content[" + std::to_string(content_index) +
                                    "].type",
                                target);
    if (!type) {
      return std::unexpected(std::move(type).error());
    }
    if (*type == "output_text") {
      auto text = required_string(block,
                                  "text",
                                  "output[" + std::to_string(item_index) + "].content[" +
                                      std::to_string(content_index) + "].text",
                                  target);
      if (!text) {
        return std::unexpected(std::move(text).error());
      }
      blocks.emplace_back(core::TextContent{.text = std::move(*text)});
      continue;
    }
    return std::unexpected(
        parse_error("openai message content block type is not supported",
                    "output[" + std::to_string(item_index) + "].content[" + std::to_string(content_index) + "].type",
                    target)
            .with("type", std::move(*type)));
  }
  return {};
}

[[nodiscard]] core::Result<void> append_openai_reasoning(const json& item,
                                                         std::size_t item_index,
                                                         std::vector<core::Content>& blocks,
                                                         const ModelTarget& target) {
  const auto* summary = member(item, "summary");
  if (summary == nullptr || summary->is_null()) {
    return {};
  }
  if (!summary->is_array()) {
    return std::unexpected(parse_error("openai reasoning summary must be an array",
                                       "output[" + std::to_string(item_index) + "].summary",
                                       target));
  }
  for (std::size_t index = 0; index < summary->size(); ++index) {
    const auto& entry = (*summary)[index];
    if (!entry.is_object()) {
      return std::unexpected(
          parse_error("openai reasoning summary entry must be an object",
                      "output[" + std::to_string(item_index) + "].summary[" + std::to_string(index) + "]",
                      target));
    }
    auto type =
        required_string(entry,
                        "type",
                        "output[" + std::to_string(item_index) + "].summary[" + std::to_string(index) + "].type",
                        target);
    if (!type) {
      return std::unexpected(std::move(type).error());
    }
    if (*type != "summary_text") {
      continue;
    }
    auto text =
        required_string(entry,
                        "text",
                        "output[" + std::to_string(item_index) + "].summary[" + std::to_string(index) + "].text",
                        target);
    if (!text) {
      return std::unexpected(std::move(text).error());
    }
    blocks.emplace_back(core::ThinkingContent{.thinking = std::move(*text), .signature = std::nullopt});
  }
  return {};
}

[[nodiscard]] core::Result<void> append_openai_function_call(const json& item,
                                                             std::size_t item_index,
                                                             std::vector<core::Content>& blocks,
                                                             const ModelTarget& target) {
  auto call_id = required_string(item, "call_id", "output[" + std::to_string(item_index) + "].call_id", target);
  if (!call_id) {
    return std::unexpected(std::move(call_id).error());
  }
  auto name = required_string(item, "name", "output[" + std::to_string(item_index) + "].name", target);
  if (!name) {
    return std::unexpected(std::move(name).error());
  }
  auto arguments = required_string(item, "arguments", "output[" + std::to_string(item_index) + "].arguments", target);
  if (!arguments) {
    return std::unexpected(std::move(arguments).error());
  }
  auto parsed_arguments = parse_json_object(*arguments, "output[" + std::to_string(item_index) + "].arguments", target);
  if (!parsed_arguments) {
    return std::unexpected(std::move(parsed_arguments).error().with("tool_use_id", *call_id).with("tool", *name));
  }
  blocks.emplace_back(core::ToolUseContent{
      .id = std::move(*call_id),
      .name = std::move(*name),
      .input_json = parsed_arguments->dump(),
  });
  return {};
}

[[nodiscard]] core::Result<Usage> openai_usage(const json& body, const ModelTarget& target) {
  const auto* usage = member(body, "usage");
  if (usage == nullptr || usage->is_null()) {
    return Usage{};
  }
  if (!usage->is_object()) {
    return std::unexpected(parse_error("openai usage field must be an object", "usage", target));
  }

  auto input = optional_u64(*usage, "input_tokens", "usage.input_tokens", target);
  if (!input) {
    return std::unexpected(std::move(input).error());
  }
  auto output = optional_u64(*usage, "output_tokens", "usage.output_tokens", target);
  if (!output) {
    return std::unexpected(std::move(output).error());
  }

  std::uint64_t cache_read = 0;
  if (const auto* details = member(*usage, "input_tokens_details"); details != nullptr && !details->is_null()) {
    if (!details->is_object()) {
      return std::unexpected(
          parse_error("openai input token details must be an object", "usage.input_tokens_details", target));
    }
    auto cached = optional_u64(*details, "cached_tokens", "usage.input_tokens_details.cached_tokens", target);
    if (!cached) {
      return std::unexpected(std::move(cached).error());
    }
    cache_read = *cached;
  }

  return Usage{
      .input_tokens = *input,
      .output_tokens = *output,
      .cache_creation_tokens = 0,
      .cache_read_tokens = cache_read,
      .cost_estimate = std::nullopt,
  };
}

[[nodiscard]] core::StopReason openai_status_stop_reason(std::string_view status, bool has_tool_use, const json& body) {
  if (status == "completed") {
    return has_tool_use ? core::StopReason::tool_use : core::StopReason::end_turn;
  }
  if (status == "cancelled") {
    return core::StopReason::cancelled;
  }
  if (status == "incomplete") {
    const auto* details = member(body, "incomplete_details");
    if (details != nullptr && details->is_object()) {
      const auto* reason = member(*details, "reason");
      if (reason != nullptr && reason->is_string() && reason->get<std::string_view>() == "max_output_tokens") {
        return core::StopReason::max_tokens;
      }
    }
    return core::StopReason::error;
  }
  return core::StopReason::error;
}

[[nodiscard]] core::Result<Response> decode_openai_response(std::string_view body_json, const ModelTarget& target) {
  auto body = parse_body_json(body_json, target);
  if (!body) {
    return std::unexpected(std::move(body).error());
  }

  auto output = required_array(*body, "output", "output", target);
  if (!output) {
    return std::unexpected(std::move(output).error());
  }

  std::vector<core::Content> blocks;
  bool has_tool_use = false;
  for (std::size_t index = 0; index < output->size(); ++index) {
    const auto& item = (*output)[index];
    if (!item.is_object()) {
      return std::unexpected(
          parse_error("openai output item must be an object", "output[" + std::to_string(index) + "]", target));
    }
    auto type = required_string(item, "type", "output[" + std::to_string(index) + "].type", target);
    if (!type) {
      return std::unexpected(std::move(type).error());
    }
    if (*type == "message") {
      auto appended = append_openai_message_content(item, index, blocks, target);
      if (!appended) {
        return std::unexpected(std::move(appended).error());
      }
      continue;
    }
    if (*type == "reasoning") {
      auto appended = append_openai_reasoning(item, index, blocks, target);
      if (!appended) {
        return std::unexpected(std::move(appended).error());
      }
      continue;
    }
    if (*type == "function_call") {
      auto appended = append_openai_function_call(item, index, blocks, target);
      if (!appended) {
        return std::unexpected(std::move(appended).error());
      }
      has_tool_use = true;
      continue;
    }
    return std::unexpected(
        parse_error("openai output item type is not supported", "output[" + std::to_string(index) + "].type", target)
            .with("type", std::move(*type)));
  }

  auto status = required_string(*body, "status", "status", target);
  if (!status) {
    return std::unexpected(std::move(status).error());
  }
  auto usage = openai_usage(*body, target);
  if (!usage) {
    return std::unexpected(std::move(usage).error());
  }
  auto model = optional_string(*body, "model", "model", target);
  if (!model) {
    return std::unexpected(std::move(model).error());
  }

  return Response{
      .blocks = std::move(blocks),
      .stop_reason = openai_status_stop_reason(*status, has_tool_use, *body),
      .usage = *usage,
      .model_used = model->empty() ? std::nullopt : std::optional<std::string>{std::move(*model)},
  };
}

}  // namespace

core::Result<Response> decode_protocol_response(std::string_view body_json, const ModelTarget& target) {
  switch (target.protocol) {
    case ProtocolKind::anthropic_messages:
      return decode_anthropic_response(body_json, target);
    case ProtocolKind::openai_responses:
      return decode_openai_response(body_json, target);
    case ProtocolKind::openai_chat_completions:
    case ProtocolKind::gemini_generate_content:
    case ProtocolKind::custom_openai_compatible:
      return std::unexpected(protocol_error("provider protocol response decoding is not implemented", target));
  }
  return std::unexpected(protocol_error("provider protocol response decoding is not implemented", target));
}

}  // namespace orangutan::provider
