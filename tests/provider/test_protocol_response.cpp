// tests/provider/test_protocol_response.cpp - provider protocol response mapping.

#include <oran/provider.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>

namespace {

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

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("protocol response decodes Anthropic Messages payloads", "[unit][provider][protocol]") {
  constexpr std::string_view body = R"json({
    "id": "msg_1",
    "type": "message",
    "role": "assistant",
    "model": "claude-sonnet-4-5",
    "content": [
      {"type": "thinking", "thinking": "checking", "signature": "sig-1"},
      {"type": "text", "text": "I will read it."},
      {"type": "tool_use", "id": "toolu_1", "name": "FileRead", "input": {"path": "README.md"}}
    ],
    "stop_reason": "tool_use",
    "stop_sequence": null,
    "usage": {
      "input_tokens": 12,
      "cache_creation_input_tokens": 3,
      "cache_read_input_tokens": 5,
      "output_tokens": 7
    }
  })json";

  auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::anthropic_messages));

  REQUIRE(decoded.has_value());
  REQUIRE(decoded->stop_reason == core::StopReason::tool_use);
  REQUIRE(decoded->model_used == std::optional<std::string>{"claude-sonnet-4-5"});
  REQUIRE(decoded->usage.input_tokens == 12);
  REQUIRE(decoded->usage.cache_creation_tokens == 3);
  REQUIRE(decoded->usage.cache_read_tokens == 5);
  REQUIRE(decoded->usage.output_tokens == 7);
  REQUIRE(decoded->blocks.size() == 3);
  REQUIRE(std::get<core::ThinkingContent>(decoded->blocks[0]).thinking == "checking");
  REQUIRE(std::get<core::ThinkingContent>(decoded->blocks[0]).signature == std::optional<std::string>{"sig-1"});
  REQUIRE(std::get<core::TextContent>(decoded->blocks[1]).text == "I will read it.");
  const auto& tool = std::get<core::ToolUseContent>(decoded->blocks[2]);
  REQUIRE(tool.id == "toolu_1");
  REQUIRE(tool.name == "FileRead");
  REQUIRE(tool.input_json == R"({"path":"README.md"})");
}

TEST_CASE("protocol response maps unknown Anthropic stop reasons to error", "[unit][provider][protocol]") {
  constexpr std::string_view body = R"json({
    "type": "message",
    "role": "assistant",
    "model": "claude-sonnet",
    "content": [],
    "stop_reason": "pause_turn",
    "usage": {}
  })json";

  auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::anthropic_messages));

  REQUIRE(decoded.has_value());
  REQUIRE(decoded->stop_reason == core::StopReason::error);
}

TEST_CASE("protocol response decodes OpenAI Responses payloads", "[unit][provider][protocol]") {
  constexpr std::string_view body = R"json({
    "id": "resp_1",
    "object": "response",
    "status": "completed",
    "model": "gpt-5.1",
    "output": [
      {
        "type": "reasoning",
        "summary": [{"type": "summary_text", "text": "Need a file read."}]
      },
      {
        "type": "message",
        "status": "completed",
        "role": "assistant",
        "content": [
          {"type": "output_text", "text": "I will inspect the file."}
        ]
      },
      {
        "type": "function_call",
        "call_id": "call_1",
        "name": "FileRead",
        "arguments": "{\"path\":\"README.md\"}"
      }
    ],
    "usage": {
      "input_tokens": 40,
      "input_tokens_details": {"cached_tokens": 8},
      "output_tokens": 9,
      "total_tokens": 49
    }
  })json";

  auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::openai_responses));

  REQUIRE(decoded.has_value());
  REQUIRE(decoded->stop_reason == core::StopReason::tool_use);
  REQUIRE(decoded->model_used == std::optional<std::string>{"gpt-5.1"});
  REQUIRE(decoded->usage.input_tokens == 40);
  REQUIRE(decoded->usage.cache_creation_tokens == 0);
  REQUIRE(decoded->usage.cache_read_tokens == 8);
  REQUIRE(decoded->usage.output_tokens == 9);
  REQUIRE(decoded->blocks.size() == 3);
  REQUIRE(std::get<core::ThinkingContent>(decoded->blocks[0]).thinking == "Need a file read.");
  REQUIRE(std::get<core::TextContent>(decoded->blocks[1]).text == "I will inspect the file.");
  const auto& tool = std::get<core::ToolUseContent>(decoded->blocks[2]);
  REQUIRE(tool.id == "call_1");
  REQUIRE(tool.name == "FileRead");
  REQUIRE(tool.input_json == R"({"path":"README.md"})");
}

TEST_CASE("protocol response maps OpenAI incomplete status to max_tokens", "[unit][provider][protocol]") {
  constexpr std::string_view body = R"json({
    "status": "incomplete",
    "model": "gpt-5.1",
    "output": [],
    "incomplete_details": {"reason": "max_output_tokens"},
    "usage": {"input_tokens": 4, "output_tokens": 2}
  })json";

  auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::openai_responses));

  REQUIRE(decoded.has_value());
  REQUIRE(decoded->stop_reason == core::StopReason::max_tokens);
}

TEST_CASE("protocol response maps OpenAI cancelled status to StopReason::cancelled", "[unit][provider][protocol]") {
  constexpr std::string_view body = R"json({
    "status": "cancelled",
    "model": "gpt-5.1",
    "output": [],
    "usage": {"input_tokens": 1, "output_tokens": 0}
  })json";

  auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::openai_responses));

  REQUIRE(decoded.has_value());
  REQUIRE(decoded->stop_reason == core::StopReason::cancelled);
}

TEST_CASE("protocol response rejects malformed protocol JSON", "[unit][provider][protocol]") {
  SECTION("body is not JSON") {
    auto decoded = provider::decode_protocol_response("{", target(provider::ProtocolKind::anthropic_messages));

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(decoded.error(), "field") == std::optional<std::string_view>{"body"});
  }

  SECTION("anthropic tool input is not an object") {
    constexpr std::string_view body = R"json({
      "type": "message",
      "role": "assistant",
      "model": "claude-sonnet",
      "content": [
        {"type": "tool_use", "id": "toolu_1", "name": "FileRead", "input": "README.md"}
      ],
      "stop_reason": "tool_use",
      "usage": {"input_tokens": 1, "output_tokens": 1}
    })json";

    auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::anthropic_messages));

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(decoded.error(), "tool_use_id") == std::optional<std::string_view>{"toolu_1"});
  }

  SECTION("openai function call arguments are not JSON") {
    constexpr std::string_view body = R"json({
      "status": "completed",
      "model": "gpt-5.1",
      "output": [
        {"type": "function_call", "call_id": "call_1", "name": "FileRead", "arguments": "not json"}
      ],
      "usage": {"input_tokens": 1, "output_tokens": 1}
    })json";

    auto decoded = provider::decode_protocol_response(body, target(provider::ProtocolKind::openai_responses));

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(decoded.error(), "field") == std::optional<std::string_view>{"output[0].arguments"});
    REQUIRE(context_value(decoded.error(), "tool_use_id") == std::optional<std::string_view>{"call_1"});
  }
}

TEST_CASE("protocol response rejects unsupported protocol families", "[unit][provider][protocol]") {
  auto unsupported = target(provider::ProtocolKind::openai_chat_completions);
  unsupported.model = "gpt-chat";

  auto decoded = provider::decode_protocol_response("{}", unsupported);

  REQUIRE_FALSE(decoded.has_value());
  REQUIRE(decoded.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(decoded.error(), "protocol") == std::optional<std::string_view>{"openai_chat_completions"});
}
