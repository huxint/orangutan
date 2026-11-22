// tests/provider/test_protocol_request.cpp - provider protocol request mapping.

#include <oran/provider.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/tool_def.hpp>

namespace {

using json = ::nlohmann::ordered_json;
namespace core = orangutan::core;
namespace provider = orangutan::provider;

provider::ModelTarget target(provider::ProtocolKind protocol) {
  return provider::ModelTarget{
      .profile = "main",
      .model = protocol == provider::ProtocolKind::anthropic_messages ? "claude-sonnet" : "gpt-responses",
      .protocol = protocol,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

core::ToolDef read_tool() {
  return core::ToolDef{
      .name = "FileRead",
      .description = "Read a file",
      .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
      .required_capabilities = {},
  };
}

provider::Request tool_request() {
  auto request = provider::Request{};
  request.system_prompt = "You are a precise coding agent.";
  request.messages = {
      core::Message{
          .role = core::Role::user,
          .blocks = {core::TextContent{.text = "Read README.md"}},
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::assistant,
          .blocks = {core::ToolUseContent{
              .id = "call-1",
              .name = "FileRead",
              .input_json = R"({"path":"README.md"})",
          }},
          .created_at = std::nullopt,
      },
      core::Message{
          .role = core::Role::tool,
          .blocks = {core::ToolResultContent{
              .tool_use_id = "call-1",
              .output = "README text fallback",
              .data_json = std::string{R"({"kind":"file_read","path":"README.md","text":"README body"})"},
              .is_error = false,
          }},
          .created_at = std::nullopt,
      },
  };
  request.tools = {read_tool()};
  request.tool_choice = std::string{"auto"};
  request.max_tokens = 256;
  request.stream = false;
  return request;
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("protocol request maps Anthropic Messages payloads", "[unit][provider][protocol]") {
  auto request = tool_request();
  request.thinking_budget = 1024;

  auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

  REQUIRE(encoded.has_value());
  REQUIRE(encoded->method == "POST");
  REQUIRE(encoded->path == "/v1/messages");

  const auto body = json::parse(encoded->body_json);
  REQUIRE(body.at("model") == "claude-sonnet");
  REQUIRE(body.at("max_tokens") == 256);
  REQUIRE(body.at("stream") == false);
  REQUIRE(body.at("system") == "You are a precise coding agent.");
  REQUIRE(body.at("thinking").at("type") == "enabled");
  REQUIRE(body.at("thinking").at("budget_tokens") == 1024);
  REQUIRE(body.at("tool_choice").at("type") == "auto");

  REQUIRE(body.at("tools").size() == 1);
  REQUIRE(body.at("tools")[0].at("name") == "FileRead");
  REQUIRE(body.at("tools")[0].at("input_schema").at("properties").contains("path"));

  REQUIRE(body.at("messages").size() == 3);
  REQUIRE(body.at("messages")[0].at("role") == "user");
  REQUIRE(body.at("messages")[0].at("content")[0].at("type") == "text");
  REQUIRE(body.at("messages")[1].at("role") == "assistant");
  REQUIRE(body.at("messages")[1].at("content")[0].at("type") == "tool_use");
  REQUIRE(body.at("messages")[1].at("content")[0].at("input").at("path") == "README.md");
  REQUIRE(body.at("messages")[2].at("role") == "user");
  REQUIRE(body.at("messages")[2].at("content")[0].at("type") == "tool_result");
  REQUIRE(body.at("messages")[2].at("content")[0].at("tool_use_id") == "call-1");
  REQUIRE(body.at("messages")[2].at("content")[0].at("content") == "README text fallback");
}

TEST_CASE("protocol request maps OpenAI Responses payloads", "[unit][provider][protocol]") {
  const auto encoded =
      provider::make_protocol_request(tool_request(), target(provider::ProtocolKind::openai_responses));

  REQUIRE(encoded.has_value());
  REQUIRE(encoded->method == "POST");
  REQUIRE(encoded->path == "/responses");

  const auto body = json::parse(encoded->body_json);
  REQUIRE(body.at("model") == "gpt-responses");
  REQUIRE(body.at("max_output_tokens") == 256);
  REQUIRE(body.at("stream") == false);
  REQUIRE(body.at("instructions") == "You are a precise coding agent.");
  REQUIRE(body.at("tool_choice") == "auto");

  REQUIRE(body.at("tools").size() == 1);
  REQUIRE(body.at("tools")[0].at("type") == "function");
  REQUIRE(body.at("tools")[0].at("name") == "FileRead");
  REQUIRE(body.at("tools")[0].at("parameters").at("required")[0] == "path");

  REQUIRE(body.at("input").size() == 3);
  REQUIRE(body.at("input")[0].at("role") == "user");
  REQUIRE(body.at("input")[0].at("content")[0].at("type") == "input_text");
  REQUIRE(body.at("input")[1].at("type") == "function_call");
  REQUIRE(body.at("input")[1].at("call_id") == "call-1");
  REQUIRE(body.at("input")[1].at("arguments") == R"({"path":"README.md"})");
  REQUIRE(body.at("input")[2].at("type") == "function_call_output");
  REQUIRE(body.at("input")[2].at("call_id") == "call-1");
  REQUIRE(body.at("input")[2].at("output") == "README text fallback");
}

TEST_CASE("protocol request folds OpenAI Role::system messages into instructions", "[unit][provider][protocol]") {
  auto request = tool_request();
  request.system_prompt = std::string{"From system_prompt."};
  request.messages.insert(request.messages.begin(),
                          core::Message{
                              .role = core::Role::system,
                              .blocks = {core::TextContent{.text = "From a system message."}},
                              .created_at = std::nullopt,
                          });

  const auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::openai_responses));

  REQUIRE(encoded.has_value());
  const auto body = json::parse(encoded->body_json);
  REQUIRE(body.at("instructions") == "From system_prompt.\n\nFrom a system message.");
  for (const auto& item : body.at("input")) {
    if (item.is_object() && item.contains("role")) {
      REQUIRE(item.at("role") != "system");
    }
  }
}

TEST_CASE("protocol request preserves text-only tool results", "[unit][provider][protocol]") {
  auto request = tool_request();
  auto& result = std::get<core::ToolResultContent>(request.messages[2].blocks[0]);
  result.data_json = std::nullopt;

  auto anthropic = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));
  REQUIRE(anthropic.has_value());
  auto anthropic_body = json::parse(anthropic->body_json);
  REQUIRE(anthropic_body.at("messages")[2].at("content")[0].at("content") == "README text fallback");

  auto openai = provider::make_protocol_request(request, target(provider::ProtocolKind::openai_responses));
  REQUIRE(openai.has_value());
  auto openai_body = json::parse(openai->body_json);
  REQUIRE(openai_body.at("input")[2].at("output") == "README text fallback");
}

TEST_CASE("protocols preserve tool guidance ahead of structured data", "[unit][provider][protocol]") {
  for (const auto protocol : {provider::ProtocolKind::anthropic_messages, provider::ProtocolKind::openai_responses}) {
    auto request = tool_request();
    auto& result = std::get<core::ToolResultContent>(request.messages[2].blocks[0]);
    result.output = "[Tool output truncated.]\nRead again with a smaller limit.";
    auto encoded = provider::make_protocol_request(request, target(protocol));
    REQUIRE(encoded.has_value());
    auto body = json::parse(encoded->body_json);
    auto output = protocol == provider::ProtocolKind::anthropic_messages
        ? body.at("messages")[2].at("content")[0].at("content")
        : body.at("input")[2].at("output");
    REQUIRE(output == result.output);

    result.output.clear();
    encoded = provider::make_protocol_request(request, target(protocol));
    REQUIRE(encoded.has_value());
    body = json::parse(encoded->body_json);
    output = protocol == provider::ProtocolKind::anthropic_messages
        ? body.at("messages")[2].at("content")[0].at("content")
        : body.at("input")[2].at("output");
    REQUIRE(output == *result.data_json);
  }
}

TEST_CASE("protocol request maps Anthropic structured tool results to text content", "[unit][provider][protocol]") {
  auto request = tool_request();
  auto& result = std::get<core::ToolResultContent>(request.messages[2].blocks[0]);

  auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

  REQUIRE(encoded.has_value());
  auto body = json::parse(encoded->body_json);
  const auto& block = body.at("messages")[2].at("content")[0];
  REQUIRE(block.at("type") == "tool_result");
  REQUIRE(block.at("content") == "README text fallback");
  REQUIRE_FALSE(block.at("content").is_array());

  result.output.clear();
  encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

  REQUIRE(encoded.has_value());
  body = json::parse(encoded->body_json);
  REQUIRE(body.at("messages")[2].at("content")[0].at("content") ==
          R"({"kind":"file_read","path":"README.md","text":"README body"})");
}

TEST_CASE("protocol request rejects malformed opaque JSON fields", "[unit][provider][protocol]") {
  SECTION("tool schema") {
    auto request = tool_request();
    request.tools[0].input_schema_json = R"(["not","an","object"])";

    auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::openai_responses));

    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(encoded.error(), "field") == std::optional<std::string_view>{"tool.input_schema_json"});
    REQUIRE(context_value(encoded.error(), "tool") == std::optional<std::string_view>{"FileRead"});
  }

  SECTION("tool input") {
    auto request = tool_request();
    std::get<core::ToolUseContent>(request.messages[1].blocks[0]).input_json = "not json";

    auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(encoded.error(), "field") == std::optional<std::string_view>{"tool.input_json"});
    REQUIRE(context_value(encoded.error(), "tool_use_id") == std::optional<std::string_view>{"call-1"});
  }

  SECTION("anthropic structured tool result fallback") {
    auto request = tool_request();
    std::get<core::ToolResultContent>(request.messages[2].blocks[0]).data_json = "{";

    auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

    REQUIRE(encoded.has_value());
    auto body = json::parse(encoded->body_json);
    REQUIRE(body.at("messages")[2].at("content")[0].at("content") == "README text fallback");
  }
}

TEST_CASE("protocol request rejects unsupported protocol families", "[unit][provider][protocol]") {
  auto unsupported = target(provider::ProtocolKind::openai_chat_completions);
  unsupported.model = "gpt-chat";

  auto encoded = provider::make_protocol_request(tool_request(), unsupported);

  REQUIRE_FALSE(encoded.has_value());
  REQUIRE(encoded.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(encoded.error(), "protocol") == std::optional<std::string_view>{"openai_chat_completions"});
}

TEST_CASE("protocol request validates provider-specific controls", "[unit][provider][protocol]") {
  SECTION("anthropic requires max_tokens") {
    auto request = tool_request();
    request.max_tokens = std::nullopt;

    auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));

    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().kind() == core::ErrorKind::config);
    REQUIRE(encoded.error().message() == "anthropic messages requests require max_tokens");
  }

  SECTION("openai responses rejects Anthropic thinking budget") {
    auto request = tool_request();
    request.thinking_budget = 2048;

    auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::openai_responses));

    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().kind() == core::ErrorKind::config);
    REQUIRE(encoded.error().message() == "openai responses requests do not accept token-budget thinking controls");
  }
}

TEST_CASE("Anthropic requests explicitly disable unbudgeted thinking", "[unit][provider][protocol]") {
  auto request = tool_request();
  request.thinking_budget.reset();
  auto encoded = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));
  REQUIRE(encoded);
  const auto body = json::parse(encoded->body_json);
  REQUIRE(body.at("thinking") == json{{"type", "disabled"}});
}

TEST_CASE("Provider requests preserve typed images without remote credential URLs", "[unit][provider][protocol]") {
  provider::Request request;
  request.max_tokens = 256;
  request.messages.push_back(
      core::Message{.role = core::Role::user,
                    .blocks = {core::TextContent{"describe"}, core::ImageContent{"image/png", "aW1hZ2U="}},
                    .created_at = std::nullopt});
  auto anthropic = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));
  REQUIRE(anthropic);
  const auto image = json::parse(anthropic->body_json).at("messages").at(0).at("content").at(1);
  CHECK(image.at("type") == "image");
  CHECK(image.at("source") == json{{"type", "base64"}, {"media_type", "image/png"}, {"data", "aW1hZ2U="}});
  auto openai = provider::make_protocol_request(request, target(provider::ProtocolKind::openai_responses));
  REQUIRE(openai);
  const auto item = json::parse(openai->body_json).at("input").at(1).at("content").at(0);
  CHECK(item.at("type") == "input_image");
  CHECK(item.at("image_url") == "data:image/png;base64,aW1hZ2U=");
  request.messages.front().blocks.back() = core::ImageContent{"application/pdf", "PRIVATE_DATA"};
  auto invalid = provider::make_protocol_request(request, target(provider::ProtocolKind::anthropic_messages));
  REQUIRE_FALSE(invalid);
  CHECK_FALSE(invalid.error().message().contains("PRIVATE_DATA"));
}
