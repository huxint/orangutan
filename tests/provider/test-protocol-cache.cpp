#include <oran/provider/protocol_request.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace core = orangutan::core;
namespace provider = orangutan::provider;
using json = nlohmann::ordered_json;

provider::ModelTarget target(provider::ProtocolKind protocol) {
  return {.profile = "test",
          .model = "test-model",
          .protocol = protocol,
          .thinking_budget = std::nullopt,
          .cache = std::nullopt};
}

provider::Request request() {
  provider::Request value;
  value.system_prompt = "stable instructions";
  value.tools = {
      {.name = "Read",
       .description = "Read a record",
       .input_schema_json = R"({"type":"object"})",
       .required_capabilities = {}},
      {.name = "Write",
       .description = "Write a record",
       .input_schema_json = R"({"type":"object"})",
       .required_capabilities = {}},
  };
  value.messages = {
      core::Message::user_text("dynamic question"),
      {.role = core::Role::assistant,
       .blocks = {core::ToolUseContent{.id = "call-1", .name = "Read", .input_json = "{}"}},
       .created_at = std::nullopt},
      {.role = core::Role::tool,
       .blocks = {core::ToolResultContent{.tool_use_id = "call-1",
                                         .output = "dynamic tool result",
                                         .data_json = std::nullopt,
                                         .is_error = false}},
       .created_at = std::nullopt},
  };
  value.max_tokens = 64;
  value.cache = provider::PromptCacheHints{
      .prefix_hash = 0xC0FFEE,
      .prefix_bytes = std::ranges::fold_left(value.tools,
                                            value.system_prompt->size(),
                                            [](std::size_t size, const core::ToolDef& tool) {
                                              return size + tool.name.size() + tool.description.size() +
                                                     tool.input_schema_json.size();
                                            }),
  };
  return value;
}

json encode(const provider::Request& value, const provider::ModelTarget& model) {
  const auto encoded = provider::make_protocol_request(value, model);
  REQUIRE(encoded.has_value());
  return json::parse(encoded->body_json);
}

}  // namespace

TEST_CASE("protocol cache controls honor the selected profile and declared prefix", "[unit][provider][cache]") {
  const auto protocol =
      GENERATE(provider::ProtocolKind::anthropic_messages, provider::ProtocolKind::openai_responses);
  auto model = target(protocol);
  auto value = request();
  value.stream = GENERATE(false, true);
  bool enabled = true;

  SECTION("default policy enables an available prefix") {}
  SECTION("the byte floor includes native tool declarations") {
    model.cache = provider::PromptCacheOptions{.min_prefix_bytes = value.cache->prefix_bytes};
    REQUIRE(model.cache->min_prefix_bytes > value.system_prompt->size());
  }
  SECTION("a prefix below the byte floor omits controls") {
    model.cache = provider::PromptCacheOptions{.min_prefix_bytes = value.cache->prefix_bytes + 1};
    enabled = false;
  }
  SECTION("an explicitly disabled profile omits controls") {
    model.cache = provider::PromptCacheOptions{.enabled = false};
    enabled = false;
  }
  SECTION("missing hints omit controls") {
    value.cache.reset();
    enabled = false;
  }
  SECTION("a zero-byte prefix omits controls") {
    value.cache->prefix_bytes = 0;
    enabled = false;
  }
  SECTION("conversation system text cannot substitute for a stable prefix") {
    value.system_prompt.reset();
    value.tools.clear();
    value.messages.insert(value.messages.begin(),
                          core::Message{.role = core::Role::system,
                                        .blocks = {core::TextContent{.text = "dynamic system text"}},
                                        .created_at = std::nullopt});
    enabled = false;
  }

  const auto body = encode(value, model);
  auto without_hints = value;
  without_hints.cache.reset();
  const auto baseline = encode(without_hints, model);
  CHECK(body.at("stream") == value.stream);
  if (!enabled) {
    CHECK(body == baseline);
  } else if (protocol == provider::ProtocolKind::anthropic_messages) {
    REQUIRE(body.at("system").is_array());
    CHECK(body.at("system")[0].at("text") == *value.system_prompt);
    CHECK(body.at("system")[0].at("cache_control") == json{{"type", "ephemeral"}});
    CHECK(body.at("tools") == baseline.at("tools"));
    CHECK(body.at("messages") == baseline.at("messages"));
    CHECK_FALSE(body.contains("prompt_cache_key"));
  } else {
    CHECK(body.at("prompt_cache_key") == "oran-0000000000c0ffee");
    auto without_key = body;
    without_key.erase("prompt_cache_key");
    CHECK(without_key == baseline);
  }
}

TEST_CASE("Anthropic caches stable system text before lifted conversation instructions", "[unit][provider][cache]") {
  const auto model = target(provider::ProtocolKind::anthropic_messages);
  auto value = request();
  value.messages.insert(value.messages.begin(),
                        core::Message{.role = core::Role::system,
                                      .blocks = {core::TextContent{.text = "dynamic system text"}},
                                      .created_at = std::nullopt});
  const auto body = encode(value, model);

  REQUIRE(body.at("system").is_array());
  REQUIRE(body.at("system").size() == 2);
  CHECK(body.at("system")[0].at("text") == *value.system_prompt);
  CHECK(body.at("system")[0].at("cache_control") == json{{"type", "ephemeral"}});
  CHECK_FALSE(body.at("system")[1].contains("cache_control"));

  value.cache.reset();
  const auto baseline = encode(value, model);
  CHECK(body.at("system")[0].at("text").get<std::string>() + body.at("system")[1].at("text").get<std::string>() ==
        baseline.at("system").get<std::string>());
  CHECK(body.at("messages") == baseline.at("messages"));
  CHECK(body.at("tools") == baseline.at("tools"));
}

TEST_CASE("Anthropic caches the last native tool when stable system text is absent", "[unit][provider][cache]") {
  const auto model = target(provider::ProtocolKind::anthropic_messages);
  auto value = request();
  SECTION("no system prompt") {
    value.system_prompt.reset();
  }
  SECTION("empty system prompt") {
    value.system_prompt = "";
  }
  value.messages.insert(value.messages.begin(),
                        core::Message{.role = core::Role::system,
                                      .blocks = {core::TextContent{.text = "dynamic system text"}},
                                      .created_at = std::nullopt});
  auto body = encode(value, model);

  REQUIRE(body.at("tools").size() == 2);
  CHECK_FALSE(body.at("tools")[0].contains("cache_control"));
  CHECK(body.at("tools")[1].at("cache_control") == json{{"type", "ephemeral"}});
  CHECK(body.at("system") == "dynamic system text");
  body["tools"][1].erase("cache_control");
  value.cache.reset();
  CHECK(body == encode(value, model));
}

TEST_CASE("Responses cache keys retain prefix identity across dynamic conversation", "[unit][provider][cache]") {
  const auto model = target(provider::ProtocolKind::openai_responses);
  auto value = request();
  const auto first = encode(value, model);
  value.messages.push_back(core::Message::user_text("continue after a new observation"));
  const auto next = encode(value, model);

  CHECK(first.at("prompt_cache_key") == next.at("prompt_cache_key"));
  CHECK(first.at("input") != next.at("input"));
  CHECK_FALSE(next.contains("cache_control"));
  CHECK_FALSE(next.contains("prompt_cache_retention"));

  ++value.cache->prefix_hash;
  const auto changed = encode(value, model);
  CHECK(changed.at("prompt_cache_key") != next.at("prompt_cache_key"));
}
