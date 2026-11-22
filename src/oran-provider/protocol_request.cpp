// src/oran-provider/protocol_request.cpp - provider protocol request bytes.

#include <oran/provider/protocol_request.hpp>

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/content.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>

namespace orangutan::provider {
namespace {

using json = ::nlohmann::ordered_json;
using orangutan::core::Error;

[[nodiscard]] bool use_prompt_cache(const Request& request, const ModelTarget& target) noexcept {
  const auto options = target.cache.value_or(PromptCacheOptions{});
  return request.cache.has_value() && options.enabled && request.cache->prefix_bytes > 0 &&
         request.cache->prefix_bytes >= options.min_prefix_bytes &&
         ((request.system_prompt.has_value() && !request.system_prompt->empty()) || !request.tools.empty());
}

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
parse_json_document(std::string_view text, std::string field, const ModelTarget& target) {
  try {
    return json::parse(text.begin(), text.end());
  } catch (const json::parse_error& error) {
    return std::unexpected(parse_error("provider protocol field is not valid JSON", std::move(field), target)
                               .with("detail", error.what()));
  } catch (const json::type_error& error) {
    return std::unexpected(parse_error("provider protocol field is not valid JSON", std::move(field), target)
                               .with("detail", error.what()));
  }
}

[[nodiscard]] core::Result<json>
parse_object_document(std::string_view text, std::string field, const ModelTarget& target) {
  auto parsed = parse_json_document(text, field, target);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }
  if (!parsed->is_object()) {
    return std::unexpected(parse_error("provider protocol field must be a JSON object", std::move(field), target));
  }
  return parsed;
}

[[nodiscard]] core::Result<json> tool_schema_json(const core::ToolDef& tool, const ModelTarget& target) {
  auto schema = parse_object_document(tool.input_schema_json, "tool.input_schema_json", target);
  if (!schema) {
    return std::unexpected(std::move(schema).error().with("tool", tool.name));
  }
  return schema;
}

[[nodiscard]] core::Result<json> tool_input_json(const core::ToolUseContent& tool, const ModelTarget& target) {
  auto input = parse_object_document(tool.input_json, "tool.input_json", target);
  if (!input) {
    return std::unexpected(std::move(input).error().with("tool", tool.name).with("tool_use_id", tool.id));
  }
  return input;
}

[[nodiscard]] const std::string& tool_result_text(const core::ToolResultContent& result) {
  return !result.output.empty() || !result.data_json.has_value() ? result.output : *result.data_json;
}

[[nodiscard]] json anthropic_tool_choice(std::string_view choice) {
  if (choice == "auto") {
    return json{{"type", "auto"}};
  }
  if (choice == "any") {
    return json{{"type", "any"}};
  }
  return json{{"type", "tool"}, {"name", std::string{choice}}};
}

[[nodiscard]] json openai_tool_choice(std::string_view choice) {
  if (choice == "auto" || choice == "none" || choice == "required") {
    return std::string{choice};
  }
  if (choice == "any") {
    return "required";
  }
  return json{{"type", "function"}, {"name", std::string{choice}}};
}

bool valid_image(const core::ImageContent& image) {
  return !image.data_base64.empty() && (image.media_type == "image/jpeg" || image.media_type == "image/png" ||
                                        image.media_type == "image/gif" || image.media_type == "image/webp");
}

[[nodiscard]] core::Result<json>
anthropic_content_block(const core::Content& content, core::Role role, const ModelTarget& target) {
  if (const auto* text = std::get_if<core::TextContent>(&content); text != nullptr) {
    return json{{"type", "text"}, {"text", text->text}};
  }
  if (const auto* image = std::get_if<core::ImageContent>(&content)) {
    if (role != core::Role::user || !valid_image(*image))
      return std::unexpected(protocol_error("unsupported or empty image input", target));
    return json{{"type", "image"},
                {"source", {{"type", "base64"}, {"media_type", image->media_type}, {"data", image->data_base64}}}};
  }
  if (const auto* thinking = std::get_if<core::ThinkingContent>(&content); thinking != nullptr) {
    auto block = json{{"type", "thinking"}, {"thinking", thinking->thinking}};
    if (thinking->signature.has_value()) {
      block["signature"] = *thinking->signature;
    }
    return block;
  }
  if (const auto* tool = std::get_if<core::ToolUseContent>(&content); tool != nullptr) {
    auto input = tool_input_json(*tool, target);
    if (!input) {
      return std::unexpected(std::move(input).error());
    }
    return json{{"type", "tool_use"}, {"id", tool->id}, {"name", tool->name}, {"input", std::move(*input)}};
  }
  const auto& result = std::get<core::ToolResultContent>(content);
  auto block = json{{"type", "tool_result"}, {"tool_use_id", result.tool_use_id}};
  block["content"] = tool_result_text(result);
  if (result.is_error) {
    block["is_error"] = true;
  }
  return block;
}

[[nodiscard]] core::Result<void>
append_anthropic_system_text(json& body, const core::Message& message, const ModelTarget& target) {
  auto parts = std::string{};
  for (const auto& block : message.blocks) {
    if (const auto* text = std::get_if<core::TextContent>(&block); text != nullptr) {
      if (!parts.empty()) {
        parts.append("\n\n");
      }
      parts.append(text->text);
      continue;
    }
    return std::unexpected(protocol_error("anthropic system messages must contain only text blocks", target));
  }

  if (parts.empty()) {
    return {};
  }
  if (body.contains("system")) {
    if (body["system"].is_array()) {
      body["system"].push_back(json{{"type", "text"}, {"text", "\n\n" + parts}});
    } else {
      body["system"] = body["system"].get<std::string>() + "\n\n" + parts;
    }
  } else {
    body["system"] = std::move(parts);
  }
  return {};
}

[[nodiscard]] core::Result<json> anthropic_message_json(const core::Message& message, const ModelTarget& target) {
  auto content = json::array();
  for (const auto& block : message.blocks) {
    auto encoded = anthropic_content_block(block, message.role, target);
    if (!encoded) {
      return std::unexpected(std::move(encoded).error());
    }
    content.push_back(std::move(*encoded));
  }

  auto role = std::string{};
  switch (message.role) {
    case core::Role::user:
      role = "user";
      break;
    case core::Role::assistant:
      role = "assistant";
      break;
    case core::Role::tool:
      role = "user";
      break;
    case core::Role::system:
      return std::unexpected(protocol_error("anthropic system messages must be lifted before encoding", target));
  }

  return json{{"role", std::move(role)}, {"content", std::move(content)}};
}

[[nodiscard]] core::Result<json> anthropic_tools_json(const Request& request, const ModelTarget& target) {
  auto tools = json::array();
  for (const auto& tool : request.tools) {
    auto schema = tool_schema_json(tool, target);
    if (!schema) {
      return std::unexpected(std::move(schema).error());
    }
    tools.push_back(json{{"name", tool.name}, {"description", tool.description}, {"input_schema", std::move(*schema)}});
  }
  return tools;
}

[[nodiscard]] core::Result<ProtocolRequest> make_anthropic_request(const Request& request, const ModelTarget& target) {
  if (target.model.empty()) {
    return std::unexpected(protocol_error("provider protocol target model must be non-empty", target));
  }
  if (!request.max_tokens.has_value()) {
    return std::unexpected(protocol_error("anthropic messages requests require max_tokens", target));
  }

  auto body = json{
      {"model", target.model},
      {"max_tokens", *request.max_tokens},
      {"stream", request.stream},
  };
  const bool cache = use_prompt_cache(request, target);
  const bool has_system_prefix = request.system_prompt.has_value() && !request.system_prompt->empty();
  if (has_system_prefix) {
    if (cache) {
      body["system"] = json::array(
          {json{{"type", "text"}, {"text", *request.system_prompt}, {"cache_control", json{{"type", "ephemeral"}}}}});
    } else {
      body["system"] = *request.system_prompt;
    }
  }
  if (request.thinking_budget.has_value()) {
    body["thinking"] = json{{"type", "enabled"}, {"budget_tokens", *request.thinking_budget}};
  } else {
    // Compatible endpoints may enable thinking by default. Absence of a local
    // budget explicitly disables it, including bounded summary requests.
    body["thinking"] = json{{"type", "disabled"}};
  }
  if (request.tool_choice.has_value()) {
    body["tool_choice"] = anthropic_tool_choice(*request.tool_choice);
  }

  auto messages = json::array();
  for (const auto& message : request.messages) {
    if (message.role == core::Role::system) {
      if (auto appended = append_anthropic_system_text(body, message, target); !appended) {
        return std::unexpected(std::move(appended).error());
      }
      continue;
    }
    auto encoded = anthropic_message_json(message, target);
    if (!encoded) {
      return std::unexpected(std::move(encoded).error());
    }
    messages.push_back(std::move(*encoded));
  }
  body["messages"] = std::move(messages);

  auto tools = anthropic_tools_json(request, target);
  if (!tools) {
    return std::unexpected(std::move(tools).error());
  }
  if (!tools->empty()) {
    if (cache && !has_system_prefix) {
      tools->back()["cache_control"] = json{{"type", "ephemeral"}};
    }
    body["tools"] = std::move(*tools);
  }

  return ProtocolRequest{.method = "POST", .path = "/v1/messages", .body_json = body.dump()};
}

[[nodiscard]] core::Result<void>
append_openai_instructions_text(json& body, const core::Message& message, const ModelTarget& target) {
  auto parts = std::string{};
  for (const auto& block : message.blocks) {
    if (const auto* text = std::get_if<core::TextContent>(&block); text != nullptr) {
      if (!parts.empty()) {
        parts.append("\n\n");
      }
      parts.append(text->text);
      continue;
    }
    return std::unexpected(protocol_error("openai system messages must contain only text blocks", target));
  }

  if (parts.empty()) {
    return {};
  }
  if (body.contains("instructions")) {
    body["instructions"] = body["instructions"].get<std::string>() + "\n\n" + parts;
  } else {
    body["instructions"] = std::move(parts);
  }
  return {};
}

[[nodiscard]] std::string openai_content_type(core::Role role) {
  return role == core::Role::assistant ? "output_text" : "input_text";
}

[[nodiscard]] json openai_text_message(core::Role role, std::string text) {
  return json{{"role", std::string{core::enum_name(role)}},
              {"content", json::array({json{{"type", openai_content_type(role)}, {"text", std::move(text)}}})}};
}

[[nodiscard]] core::Result<json>
openai_input_item(const core::Content& content, core::Role role, const ModelTarget& target) {
  if (const auto* text = std::get_if<core::TextContent>(&content); text != nullptr) {
    return openai_text_message(role, text->text);
  }
  if (const auto* image = std::get_if<core::ImageContent>(&content)) {
    if (role != core::Role::user || !valid_image(*image))
      return std::unexpected(protocol_error("unsupported image input", target));
    return json{{"role", "user"},
                {"content",
                 json::array({json{{"type", "input_image"},
                                   {"image_url", "data:" + image->media_type + ";base64," + image->data_base64}}})}};
  }
  if (const auto* thinking = std::get_if<core::ThinkingContent>(&content); thinking != nullptr) {
    return json{{"type", "reasoning"},
                {"summary", json::array({json{{"type", "summary_text"}, {"text", thinking->thinking}}})}};
  }
  if (const auto* tool = std::get_if<core::ToolUseContent>(&content); tool != nullptr) {
    auto input = tool_input_json(*tool, target);
    if (!input) {
      return std::unexpected(std::move(input).error());
    }
    return json{{"type", "function_call"}, {"call_id", tool->id}, {"name", tool->name}, {"arguments", input->dump()}};
  }

  const auto& result = std::get<core::ToolResultContent>(content);
  auto item = json{{"type", "function_call_output"}, {"call_id", result.tool_use_id}};
  item["output"] = tool_result_text(result);
  if (result.is_error) {
    item["status"] = "error";
  }
  return item;
}

[[nodiscard]] core::Result<json> openai_tools_json(const Request& request, const ModelTarget& target) {
  auto tools = json::array();
  for (const auto& tool : request.tools) {
    auto schema = tool_schema_json(tool, target);
    if (!schema) {
      return std::unexpected(std::move(schema).error());
    }
    tools.push_back(json{{"type", "function"},
                         {"name", tool.name},
                         {"description", tool.description},
                         {"parameters", std::move(*schema)}});
  }
  return tools;
}

[[nodiscard]] core::Result<ProtocolRequest> make_openai_responses_request(const Request& request,
                                                                          const ModelTarget& target) {
  if (target.model.empty()) {
    return std::unexpected(protocol_error("provider protocol target model must be non-empty", target));
  }
  if (request.thinking_budget.has_value()) {
    return std::unexpected(
        protocol_error("openai responses requests do not accept token-budget thinking controls", target));
  }

  auto body = json{{"model", target.model}, {"stream", request.stream}};
  if (use_prompt_cache(request, target)) {
    body["prompt_cache_key"] = std::format("oran-{:016x}", request.cache->prefix_hash);
  }
  if (request.system_prompt.has_value() && !request.system_prompt->empty()) {
    body["instructions"] = *request.system_prompt;
  }
  if (request.max_tokens.has_value()) {
    body["max_output_tokens"] = *request.max_tokens;
  }
  if (request.tool_choice.has_value()) {
    body["tool_choice"] = openai_tool_choice(*request.tool_choice);
  }

  auto input = json::array();
  for (const auto& message : request.messages) {
    if (message.role == core::Role::system) {
      if (auto appended = append_openai_instructions_text(body, message, target); !appended) {
        return std::unexpected(std::move(appended).error());
      }
      continue;
    }
    for (const auto& block : message.blocks) {
      auto item = openai_input_item(block, message.role, target);
      if (!item) {
        return std::unexpected(std::move(item).error());
      }
      input.push_back(std::move(*item));
    }
  }
  body["input"] = std::move(input);

  auto tools = openai_tools_json(request, target);
  if (!tools) {
    return std::unexpected(std::move(tools).error());
  }
  if (!tools->empty()) {
    body["tools"] = std::move(*tools);
  }

  return ProtocolRequest{.method = "POST", .path = "/responses", .body_json = body.dump()};
}

}  // namespace

core::Result<ProtocolRequest> make_protocol_request(const Request& request, const ModelTarget& target) {
  switch (target.protocol) {
    case ProtocolKind::anthropic_messages:
      return make_anthropic_request(request, target);
    case ProtocolKind::openai_responses:
      return make_openai_responses_request(request, target);
    case ProtocolKind::openai_chat_completions:
    case ProtocolKind::gemini_generate_content:
    case ProtocolKind::custom_openai_compatible:
      return std::unexpected(protocol_error("provider protocol request serialization is not implemented", target));
  }
  return std::unexpected(protocol_error("provider protocol request serialization is not implemented", target));
}

}  // namespace orangutan::provider
