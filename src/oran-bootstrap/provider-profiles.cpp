#include <oran/bootstrap/provider-profiles.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/config/config.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::bootstrap {
namespace {

using orangutan::core::Error;
using provider::ModelTarget;
using provider::PromptCacheOptions;
using provider::ProtocolKind;
using provider::ProviderPricing;
using provider::ResolvedProfileTarget;

constexpr auto kAnthropicAliases = std::array<std::string_view, 2>{"anthropic", "claude"};
constexpr auto kOpenAiChatAliases = std::array<std::string_view, 3>{"openai", "openai_chat", "openai_chat_completions"};
constexpr auto kOpenAiResponsesAliases = std::array<std::string_view, 2>{"openai_responses", "openai_response"};
constexpr auto kGeminiAliases = std::array<std::string_view, 3>{"gemini", "google_gemini", "gemini_generate_content"};
constexpr auto kOpenAiCompatibleAliases = std::array<std::string_view, 5>{"custom_openai_compatible",
                                                                          "openai_compatible",
                                                                          "deepseek",
                                                                          "deepseek_chat",
                                                                          "local"};

[[nodiscard]] std::optional<ProtocolKind> protocol_for_provider(std::string_view provider) noexcept {
  if (auto parsed = core::parse_enum<ProtocolKind>(provider)) {
    return parsed;
  }
  if (std::ranges::contains(kAnthropicAliases, provider)) {
    return ProtocolKind::anthropic_messages;
  }
  if (std::ranges::contains(kOpenAiChatAliases, provider)) {
    return ProtocolKind::openai_chat_completions;
  }
  if (std::ranges::contains(kOpenAiResponsesAliases, provider)) {
    return ProtocolKind::openai_responses;
  }
  if (std::ranges::contains(kGeminiAliases, provider)) {
    return ProtocolKind::gemini_generate_content;
  }
  if (std::ranges::contains(kOpenAiCompatibleAliases, provider)) {
    return ProtocolKind::custom_openai_compatible;
  }
  return std::nullopt;
}

[[nodiscard]] core::Result<ProtocolKind>
resolve_protocol(const config::ProfileConfig& profile, std::string_view route_name, std::string_view role) {
  if (profile.protocol.has_value()) {
    auto parsed = core::parse_enum<ProtocolKind>(*profile.protocol);
    if (!parsed) {
      return std::unexpected(Error::config("unknown provider protocol")
                                 .with("route", std::string{route_name})
                                 .with("profile", profile.name)
                                 .with("role", std::string{role})
                                 .with("protocol", *profile.protocol));
    }
    return *parsed;
  }

  auto protocol = protocol_for_provider(profile.provider);
  if (!protocol) {
    return std::unexpected(Error::config("unknown provider protocol")
                               .with("route", std::string{route_name})
                               .with("profile", profile.name)
                               .with("role", std::string{role})
                               .with("provider", profile.provider));
  }
  return *protocol;
}

[[nodiscard]] ProviderPricing pricing_for(const config::ProviderPricingConfig& pricing) noexcept {
  return ProviderPricing{
      .input_per_million_usd = pricing.input_per_million_usd,
      .output_per_million_usd = pricing.output_per_million_usd,
      .cache_creation_per_million_usd = pricing.cache_creation_per_million_usd,
      .cache_read_per_million_usd = pricing.cache_read_per_million_usd,
  };
}

[[nodiscard]] const config::RouteConfig* find_route(const config::Config& config, std::string_view route_name) {
  const auto routes = config.routes();
  const auto it = std::ranges::find(routes, route_name, &config::RouteConfig::name);
  return it == routes.end() ? nullptr : std::addressof(*it);
}

[[nodiscard]] const config::ProfileConfig* find_profile(const config::Config& config, std::string_view profile_name) {
  const auto profiles = config.profiles();
  const auto it = std::ranges::find(profiles, profile_name, &config::ProfileConfig::name);
  return it == profiles.end() ? nullptr : std::addressof(*it);
}

[[nodiscard]] core::Result<ResolvedProfileTarget> target_for_profile(const config::Config& config,
                                                                     std::string_view profile_name,
                                                                     std::string_view route_name,
                                                                     std::string_view role) {
  const auto* profile = find_profile(config, profile_name);
  if (profile == nullptr) {
    return std::unexpected(Error::config("route references an unknown provider profile")
                               .with("route", std::string{route_name})
                               .with("profile", std::string{profile_name})
                               .with("role", std::string{role}));
  }

  auto protocol = resolve_protocol(*profile, route_name, role);
  if (!protocol) {
    return std::unexpected(std::move(protocol).error());
  }

  std::optional<PromptCacheOptions> cache;
  if (profile->cache.has_value()) {
    cache = PromptCacheOptions{
        .enabled = profile->cache->enabled,
        .min_prefix_bytes = static_cast<std::size_t>(profile->cache->min_prefix_bytes),
    };
  }

  return ResolvedProfileTarget{
      .target =
          ModelTarget{
              .profile = profile->name,
              .model = profile->model,
              .protocol = *protocol,
              .thinking_budget = profile->thinking_budget,
              .cache = cache,
              .pricing = pricing_for(profile->pricing),
          },
      .base_url = profile->base_url,
      .api_key_env = profile->api_key_env,
  };
}

}  // namespace

core::Result<provider::RouteProfileResolution>
resolve_route_profiles(const config::Config& config, std::string_view route_name) {
  if (route_name.empty()) {
    return std::unexpected(Error::invalid_argument("route name must be non-empty"));
  }

  const auto* route = find_route(config, route_name);
  if (route == nullptr) {
    return std::unexpected(Error::config("provider route not found").with("route", std::string{route_name}));
  }

  auto primary = target_for_profile(config, route->primary_profile, route->name, "primary");
  if (!primary) {
    return std::unexpected(std::move(primary).error());
  }

  auto fallbacks = std::vector<ResolvedProfileTarget>{};
  fallbacks.reserve(route->fallback_profiles.size());
  for (const auto& fallback_profile : route->fallback_profiles) {
    auto fallback = target_for_profile(config, fallback_profile, route->name, "fallback");
    if (!fallback) {
      return std::unexpected(std::move(fallback).error());
    }
    fallbacks.push_back(std::move(*fallback));
  }

  return provider::RouteProfileResolution{
      .primary = std::move(*primary),
      .fallbacks = std::move(fallbacks),
  };
}

}  // namespace orangutan::bootstrap
