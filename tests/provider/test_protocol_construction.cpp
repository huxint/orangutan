#include <oran/provider/protocol_transport.hpp>

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::optional<std::string> value) : name_{std::move(name)} {
    if (const auto* previous = std::getenv(name_.c_str())) {
      previous_ = previous;
    }
    if (value) {
      REQUIRE(setenv(name_.c_str(), value->c_str(), 1) == 0);
    } else {
      REQUIRE(unsetenv(name_.c_str()) == 0);
    }
  }

  ~ScopedEnv() {
    if (previous_) {
      setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

provider::RouteProfileResolution route_profiles() {
  return {
      .primary =
          {
              .target = {.profile = "anthropic-main",
                         .model = "claude-sonnet",
                         .protocol = provider::ProtocolKind::anthropic_messages,
                         .thinking_budget = std::nullopt,
                         .cache = std::nullopt},
              .base_url = "https://api.anthropic.com",
              .api_key_env = "ORAN_PROVIDER_TEST_ANTHROPIC_KEY",
          },
      .fallbacks = {{
          .target = {.profile = "openai-main",
                     .model = "gpt-main",
                     .protocol = provider::ProtocolKind::openai_responses,
                     .thinking_budget = std::nullopt,
                     .cache = std::nullopt},
          .base_url = "https://api.openai.com/v1",
          .api_key_env = "ORAN_PROVIDER_TEST_OPENAI_KEY",
      }},
  };
}

class RecordingTransport final : public provider::ProtocolTransport {
public:
  async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    requests.push_back(std::move(request));
    co_return provider::ProtocolHttpResponse{
        .status_code = 200,
        .headers = {},
        .body_json =
            requests.back().url.ends_with("/messages")
                ? R"({"type":"message","role":"assistant","content":[{"type":"text","text":"anthropic ok"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"
                : R"({"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"openai ok"}]}],"usage":{"input_tokens":1,"output_tokens":1}})",
    };
  }

  mutable std::vector<provider::ProtocolHttpRequest> requests;
};

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  return it == error.context().end() ? std::nullopt : std::optional<std::string_view>{it->second};
}

}  // namespace

TEST_CASE("protocol construction rejects incomplete endpoints before credential lookup",
          "[unit][provider][construction]") {
  auto profiles = route_profiles();
  const bool fallback = GENERATE(false, true);
  const std::string_view field = GENERATE("profile", "model", "base_url", "api_key_env");
  auto& profile = fallback ? profiles.fallbacks.front() : profiles.primary;
  if (field == "profile") {
    profile.target.profile.clear();
  } else if (field == "model") {
    profile.target.model.clear();
  } else if (field == "base_url") {
    profile.base_url.clear();
  } else {
    profile.api_key_env.clear();
  }
  RecordingTransport transport;
  std::size_t lookups = 0;

  auto system = provider::make_protocol_system(transport,
                                               std::move(profiles),
                                               [&](std::string_view) -> core::Result<std::string> {
                                                 ++lookups;
                                                 return "test-secret";
                                               });

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == (fallback ? "fallback" : "primary"));
  REQUIRE(context_value(system.error(), "field") == field);
  REQUIRE(lookups == 0);
  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol construction rejects non-HTTP fallback endpoints before credential lookup",
          "[unit][provider][construction]") {
  auto profiles = route_profiles();
  profiles.fallbacks.front().base_url = "ftp://example.invalid";
  RecordingTransport transport;
  std::size_t lookups = 0;

  auto system = provider::make_protocol_system(transport,
                                               std::move(profiles),
                                               [&](std::string_view) -> core::Result<std::string> {
                                                 ++lookups;
                                                 return "test-secret";
                                               });

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == "fallback");
  REQUIRE(context_value(system.error(), "profile") == "openai-main");
  REQUIRE(context_value(system.error(), "field") == "base_url");
  REQUIRE(lookups == 0);
  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol construction rejects unsupported protocols before credential lookup",
          "[unit][provider][construction]") {
  auto profiles = route_profiles();
  profiles.fallbacks.front().target.protocol = GENERATE(provider::ProtocolKind::openai_chat_completions,
                                                        provider::ProtocolKind::gemini_generate_content,
                                                        provider::ProtocolKind::custom_openai_compatible,
                                                        static_cast<provider::ProtocolKind>(255));
  RecordingTransport transport;
  std::size_t lookups = 0;

  auto system = provider::make_protocol_system(transport,
                                               std::move(profiles),
                                               [&](std::string_view) -> core::Result<std::string> {
                                                 ++lookups;
                                                 return "test-secret";
                                               });

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == "fallback");
  REQUIRE(context_value(system.error(), "profile") == "openai-main");
  REQUIRE(lookups == 0);
  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol construction rejects duplicate profiles before credential lookup",
          "[unit][provider][construction]") {
  auto profiles = route_profiles();
  std::string duplicate;
  SECTION("fallback repeats primary") {
    duplicate = "anthropic-main";
    profiles.fallbacks.front().target.profile = duplicate;
  }
  SECTION("fallback repeats another fallback") {
    duplicate = "openai-main";
    profiles.fallbacks.push_back(profiles.fallbacks.front());
  }
  RecordingTransport transport;
  std::size_t lookups = 0;

  auto system = provider::make_protocol_system(transport,
                                               std::move(profiles),
                                               [&](std::string_view) -> core::Result<std::string> {
                                                 ++lookups;
                                                 return "test-secret";
                                               });

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(system.error(), "role") == "fallback");
  REQUIRE(context_value(system.error(), "profile") == duplicate);
  REQUIRE(lookups == 0);
  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol construction owns environment credentials for every profile", "[unit][provider][credentials]") {
  RecordingTransport transport;
  auto profiles = route_profiles();
  const auto route = profiles.route();
  std::unique_ptr<provider::System> system;
  {
    ScopedEnv primary_key{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY", "anthropic-secret"};
    ScopedEnv fallback_key{"ORAN_PROVIDER_TEST_OPENAI_KEY", "openai-secret"};
    auto created = provider::make_protocol_system(transport, std::move(profiles));
    REQUIRE(created.has_value());
    system = std::move(*created);
  }

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = provider::Request{};
    request.messages.push_back(
        core::Message{.role = core::Role::user, .blocks = {core::TextContent{.text = "hello"}}, .created_at = {}});
    request.max_tokens = 64;
    auto primary = co_await system->send(request, route.primary);
    auto fallback = co_await system->send(request, route.fallbacks.front());

    REQUIRE(primary.has_value());
    REQUIRE(fallback.has_value());
    REQUIRE(std::get<core::TextContent>(primary->blocks.front()).text == "anthropic ok");
    REQUIRE(std::get<core::TextContent>(fallback->blocks.front()).text == "openai ok");
  });

  REQUIRE(transport.requests.size() == 2);
  REQUIRE(std::ranges::contains(transport.requests[0].headers,
                                provider::ProtocolHttpHeader{"x-api-key", "anthropic-secret"}));
  REQUIRE(std::ranges::contains(transport.requests[1].headers,
                                provider::ProtocolHttpHeader{"authorization", "Bearer openai-secret"}));
}

TEST_CASE("protocol construction rejects unavailable environment credentials", "[unit][provider][credentials]") {
  ScopedEnv primary_key{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY", "anthropic-secret"};
  ScopedEnv fallback_key{"ORAN_PROVIDER_TEST_OPENAI_KEY", "openai-secret"};
  const bool fallback = GENERATE(false, true);
  const bool empty = GENERATE(false, true);
  const auto* key = fallback ? "ORAN_PROVIDER_TEST_OPENAI_KEY" : "ORAN_PROVIDER_TEST_ANTHROPIC_KEY";
  ScopedEnv unavailable{key, empty ? std::optional<std::string>{""} : std::nullopt};
  RecordingTransport transport;

  auto system = provider::make_protocol_system(transport, route_profiles());

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(system.error(), "role") == (fallback ? "fallback" : "primary"));
  REQUIRE(context_value(system.error(), "profile") == (fallback ? "openai-main" : "anthropic-main"));
  REQUIRE(context_value(system.error(), "api_key_env") == key);
  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol construction sanitizes injected credential failures", "[unit][provider][credentials]") {
  const std::string_view failure = GENERATE("error", "empty", "exception");
  RecordingTransport transport;

  auto system = provider::make_protocol_system(
      transport,
      route_profiles(),
      [&](std::string_view key) -> core::Result<std::string> {
        if (key == "ORAN_PROVIDER_TEST_ANTHROPIC_KEY") {
          return "anthropic-secret";
        }
        if (failure == "empty") {
          return "";
        }
        if (failure == "exception") {
          throw std::runtime_error{"private-lookup-detail"};
        }
        return std::unexpected(core::Error::internal("private-lookup-detail").with("key", "private-lookup-detail"));
      });

  REQUIRE_FALSE(system.has_value());
  REQUIRE(system.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(system.error(), "role") == "fallback");
  REQUIRE(context_value(system.error(), "profile") == "openai-main");
  REQUIRE(context_value(system.error(), "api_key_env") == "ORAN_PROVIDER_TEST_OPENAI_KEY");
  REQUIRE_FALSE(system.error().message().contains("private-lookup-detail"));
  for (const auto& [key, value] : system.error().context()) {
    REQUIRE_FALSE(key.contains("private-lookup-detail"));
    REQUIRE_FALSE(value.contains("private-lookup-detail"));
  }
  REQUIRE(transport.requests.empty());
}
