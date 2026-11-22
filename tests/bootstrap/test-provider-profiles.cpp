#include <oran/bootstrap/provider-profiles.hpp>

#include <algorithm>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/error.hpp>

namespace config = orangutan::config;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace bootstrap = orangutan::bootstrap;

namespace {

constexpr auto kRoutingConfig = R"json(
{
  "profiles": {
    "anthropic-main": {
      "provider": "anthropic",
      "model": "claude-sonnet",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY",
      "thinking_budget": 1024,
      "cache": {"enabled": true, "min_prefix_bytes": 2048},
      "pricing": {
        "input_per_million_usd": 3.0,
        "output_per_million_usd": 15.0,
        "cache_creation_per_million_usd": 3.75,
        "cache_read_per_million_usd": 0.3
      }
    },
    "openai-main": {
      "provider": "openai",
      "model": "gpt-main",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    },
    "responses-main": {
      "provider": "openai_responses",
      "model": "gpt-responses",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    },
    "proxied-responses": {
      "provider": "self-hosted-gateway",
      "protocol": "openai_responses",
      "model": "gpt-proxy",
      "base_url": "https://gateway.example.invalid/v1",
      "api_key_env": "GATEWAY_API_KEY"
    },
    "local-main": {
      "provider": "deepseek",
      "model": "deepseek-coder",
      "base_url": "http://127.0.0.1:11434/v1",
      "api_key_env": "LOCAL_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "anthropic-main",
      "fallbacks": ["openai-main", "local-main"]
    },
    "responses": {
      "primary": "responses-main",
      "fallbacks": []
    },
    "proxied": {
      "primary": "proxied-responses",
      "fallbacks": []
    }
  }
}
)json";

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("resolve_route_profiles preserves endpoint metadata and model policy", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto resolution = bootstrap::resolve_route_profiles(*parsed, "default");

  REQUIRE(resolution.has_value());
  REQUIRE(resolution->primary.target.profile == "anthropic-main");
  REQUIRE(resolution->primary.target.model == "claude-sonnet");
  REQUIRE(resolution->primary.target.protocol == provider::ProtocolKind::anthropic_messages);
  REQUIRE(resolution->primary.target.thinking_budget == 1024);
  REQUIRE(resolution->primary.target.cache == provider::PromptCacheOptions{true, 2048});
  REQUIRE(resolution->primary.base_url == "https://api.anthropic.com");
  REQUIRE(resolution->primary.api_key_env == "ANTHROPIC_API_KEY");
  REQUIRE(resolution->primary.target.pricing.input_per_million_usd == std::optional<double>{3.0});
  REQUIRE(resolution->primary.target.pricing.output_per_million_usd == std::optional<double>{15.0});
  REQUIRE(resolution->primary.target.pricing.cache_creation_per_million_usd == std::optional<double>{3.75});
  REQUIRE(resolution->primary.target.pricing.cache_read_per_million_usd == std::optional<double>{0.3});

  REQUIRE(resolution->fallbacks.size() == 2);
  REQUIRE(resolution->fallbacks[0].target.profile == "openai-main");
  REQUIRE(resolution->fallbacks[0].target.pricing.empty());
  REQUIRE(resolution->fallbacks[0].base_url == "https://api.openai.com/v1");
  REQUIRE(resolution->fallbacks[0].api_key_env == "OPENAI_API_KEY");
  REQUIRE(resolution->fallbacks[1].target.profile == "local-main");
  REQUIRE(resolution->fallbacks[1].base_url == "http://127.0.0.1:11434/v1");
  REQUIRE(resolution->fallbacks[1].api_key_env == "LOCAL_API_KEY");
}

TEST_CASE("RouteProfileResolution derives the loop-facing route shape", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto resolution = bootstrap::resolve_route_profiles(*parsed, "default");
  REQUIRE(resolution.has_value());

  auto route = resolution->route();

  REQUIRE(route.primary.profile == "anthropic-main");
  REQUIRE(route.primary.model == "claude-sonnet");
  REQUIRE(route.primary.protocol == provider::ProtocolKind::anthropic_messages);
  REQUIRE(route.primary.thinking_budget == 1024);
  REQUIRE(route.primary.cache == provider::PromptCacheOptions{true, 2048});
  REQUIRE(route.primary.pricing.input_per_million_usd == 3.0);
  REQUIRE(route.primary.pricing.output_per_million_usd == 15.0);
  REQUIRE(route.fallbacks.size() == 2);
  REQUIRE(route.fallbacks[0].profile == "openai-main");
  REQUIRE(route.fallbacks[0].model == "gpt-main");
  REQUIRE(route.fallbacks[0].protocol == provider::ProtocolKind::openai_chat_completions);
  REQUIRE(route.fallbacks[1].profile == "local-main");
  REQUIRE(route.fallbacks[1].model == "deepseek-coder");
  REQUIRE(route.fallbacks[1].protocol == provider::ProtocolKind::custom_openai_compatible);
}

TEST_CASE("resolve_route_profiles accepts exact protocol spellings in profile provider", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed, "responses");

  REQUIRE(route.has_value());
  REQUIRE(route->primary.target.profile == "responses-main");
  REQUIRE(route->primary.target.protocol == provider::ProtocolKind::openai_responses);
  REQUIRE(route->fallbacks.empty());
}

TEST_CASE("resolve_route_profiles prefers explicit profile protocol over provider label", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed, "proxied");

  REQUIRE(route.has_value());
  REQUIRE(route->primary.target.profile == "proxied-responses");
  REQUIRE(route->primary.target.model == "gpt-proxy");
  REQUIRE(route->primary.target.protocol == provider::ProtocolKind::openai_responses);
}

TEST_CASE("resolve_route_profiles rejects missing route names", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed, "missing");

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "route") == std::optional<std::string_view>{"missing"});
}

TEST_CASE("resolve_route_profiles rejects routes that reference unknown profiles", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(R"json(
{
  "profiles": {
    "primary": {
      "provider": "anthropic",
      "model": "claude",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "primary",
      "fallbacks": ["missing"]
    }
  }
}
)json");
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed);

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "route") == std::optional<std::string_view>{"default"});
  REQUIRE(context_value(route.error(), "profile") == std::optional<std::string_view>{"missing"});
  REQUIRE(context_value(route.error(), "role") == std::optional<std::string_view>{"fallback"});
}

TEST_CASE("resolve_route_profiles rejects unknown provider spellings", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(R"json(
{
  "profiles": {
    "bad": {
      "provider": "telepathy",
      "model": "unknown",
      "base_url": "http://127.0.0.1:1",
      "api_key_env": "BAD_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed);

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "profile") == std::optional<std::string_view>{"bad"});
  REQUIRE(context_value(route.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(route.error(), "provider") == std::optional<std::string_view>{"telepathy"});
}

TEST_CASE("resolve_route_profiles rejects unknown explicit protocols", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(R"json(
{
  "profiles": {
    "bad": {
      "provider": "openai",
      "protocol": "responses-ish",
      "model": "unknown",
      "base_url": "http://127.0.0.1:1",
      "api_key_env": "BAD_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed);

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "profile") == std::optional<std::string_view>{"bad"});
  REQUIRE(context_value(route.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(route.error(), "protocol") == std::optional<std::string_view>{"responses-ish"});
}

TEST_CASE("resolve_route_profiles rejects empty route names", "[unit][bootstrap][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = bootstrap::resolve_route_profiles(*parsed, "");

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::invalid_argument);
}
