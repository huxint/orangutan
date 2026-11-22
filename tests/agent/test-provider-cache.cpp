#include <oran/agent/loop.hpp>

#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/provider/protocol_transport.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

class CacheTransport final : public provider::ProtocolTransport {
public:
  explicit CacheTransport(std::size_t failures) : failures_{failures} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    requests.push_back(std::move(request));
    if (requests.size() <= failures_) {
      co_return provider::ProtocolHttpResponse{.status_code = 503, .headers = {}, .body_json = "{}"};
    }
    const auto body = requests.back().url.ends_with("/messages")
                          ? R"({"type":"message","role":"assistant","content":[{"type":"text","text":"done"}],)"
                            R"("stop_reason":"end_turn","usage":{"input_tokens":4,"output_tokens":1}})"
                          : R"({"status":"completed","output":[{"type":"message","role":"assistant",)"
                            R"("content":[{"type":"output_text","text":"done"}]}],)"
                            R"("usage":{"input_tokens":4,"output_tokens":1}})";
    co_return provider::ProtocolHttpResponse{.status_code = 200, .headers = {}, .body_json = body};
  }

  mutable std::vector<provider::ProtocolHttpRequest> requests;

private:
  std::size_t failures_;
};

provider::ResolvedProfileTarget profile(std::string name,
                                       provider::ProtocolKind protocol,
                                       provider::PromptCacheOptions cache) {
  return {
      .target = {.profile = std::move(name),
                 .model = "test-model",
                 .protocol = protocol,
                 .thinking_budget = std::nullopt,
                 .cache = cache},
      .base_url = "https://cache.test/v1",
      .api_key_env = "TEST_KEY",
  };
}

}  // namespace

TEST_CASE("Loop preserves prefix cache controls across retries and route policies", "[integration][agent][cache]") {
  const auto primary_protocol =
      GENERATE(provider::ProtocolKind::anthropic_messages, provider::ProtocolKind::openai_responses);
  const auto fallback_protocol = primary_protocol == provider::ProtocolKind::anthropic_messages
                                     ? provider::ProtocolKind::openai_responses
                                     : provider::ProtocolKind::anthropic_messages;
  provider::PromptCacheOptions primary_cache;
  provider::PromptCacheOptions fallback_cache;
  bool primary_enabled = true;
  bool fallback_enabled = true;
  bool third_target = false;

  SECTION("disabled primary still permits an enabled fallback") {
    primary_cache.enabled = false;
    primary_enabled = false;
  }
  SECTION("primary byte floor does not constrain a fallback") {
    primary_cache.min_prefix_bytes = std::numeric_limits<std::size_t>::max();
    primary_enabled = false;
  }
  SECTION("disabled fallback omits controls") {
    fallback_cache.enabled = false;
    fallback_enabled = false;
  }
  SECTION("fallback byte floor omits controls") {
    fallback_cache.min_prefix_bytes = std::numeric_limits<std::size_t>::max();
    fallback_enabled = false;
  }
  SECTION("an enabled target after a disabled fallback retains the prefix") {
    fallback_cache.enabled = false;
    fallback_enabled = false;
    third_target = true;
  }

  test::run_async([primary_protocol,
                   fallback_protocol,
                   primary_cache,
                   fallback_cache,
                   primary_enabled,
                   fallback_enabled,
                   third_target](asio::io_context&) -> async::Awaitable<void> {
    CacheTransport transport{third_target ? 4U : 2U};
    provider::RouteProfileResolution profiles{
        .primary = profile("primary", primary_protocol, primary_cache),
        .fallbacks = {profile("fallback", fallback_protocol, fallback_cache)},
    };
    if (third_target) {
      profiles.fallbacks.push_back(profile("final", primary_protocol, {}));
    }
    auto route = profiles.route();
    auto system = provider::make_protocol_system(
        transport, std::move(profiles), [](std::string_view) -> core::Result<std::string> { return "test-key"; });
    REQUIRE(system.has_value());
    agent::Loop loop{**system, std::move(route)};
    const std::vector<core::Message> messages{core::Message::user_text("dynamic conversation")};
    const std::vector<core::ToolDef> tools{
        {.name = "Lookup",
         .description = "Find a record",
         .input_schema_json = R"({"type":"object"})",
         .required_capabilities = {}},
    };
    const auto result = co_await loop.run_turn({
        .system_preamble = "stable instructions",
        .tool_catalog = tools,
        .conversation_tail = messages,
        .max_tokens = 64,
        .retry = {.max_attempts = 2},
        .stream = false,
    });

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    REQUIRE(transport.requests.size() == (third_target ? 5U : 3U));
    CHECK(transport.requests[0].body_json == transport.requests[1].body_json);
    if (third_target) {
      CHECK(transport.requests[2].body_json == transport.requests[3].body_json);
    }
    for (std::size_t i = 0; i < transport.requests.size(); ++i) {
      CAPTURE(i);
      const auto& request = transport.requests[i];
      const bool enabled = i < 2 ? primary_enabled : (i < 4 ? fallback_enabled : true);
      if (request.url.ends_with("/messages")) {
        CHECK(request.body_json.contains(R"("cache_control":{"type":"ephemeral"})") == enabled);
        CHECK_FALSE(request.body_json.contains("prompt_cache_key"));
      } else {
        const auto key = std::format(R"("prompt_cache_key":"oran-{:016x}")", result->rendered_prompt.prefix_hash);
        CHECK(request.body_json.contains(key) == enabled);
        CHECK_FALSE(request.body_json.contains("cache_control"));
      }
      CHECK(request.body_json.contains("stable instructions"));
      CHECK(request.body_json.contains("dynamic conversation"));
      CHECK(request.body_json.contains("Find a record"));
    }
  });
}
