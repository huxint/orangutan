#include <oran/provider/protocol_transport.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/provider/protocol_request.hpp>
#include <oran/provider/protocol_response.hpp>

#include "_impl/anthropic_sse_decoder.hpp"
#include "_impl/openai_responses_sse_decoder.hpp"

namespace orangutan::provider {
namespace {

using orangutan::core::Error;

struct Endpoint {
  ModelTarget target;
  std::string base_url;
  std::string api_key;
};

[[nodiscard]] Error config_error(std::string message, const ModelTarget& target) {
  return Error::config(std::move(message))
      .with("profile", target.profile)
      .with("model", target.model)
      .with("protocol", std::string{core::enum_name(target.protocol)});
}

[[nodiscard]] core::Result<void> validate_profile(const ResolvedProfileTarget& profile) {
  const auto fields =
      std::array<std::pair<std::string_view, std::string_view>, 4>{{{"profile", profile.target.profile},
                                                                    {"model", profile.target.model},
                                                                    {"base_url", profile.base_url},
                                                                    {"api_key_env", profile.api_key_env}}};
  for (const auto& [field, value] : fields) {
    if (value.empty()) {
      return std::unexpected(
          config_error("provider endpoint field must be non-empty", profile.target).with("field", std::string{field}));
    }
  }
  if (!profile.base_url.starts_with("http://") && !profile.base_url.starts_with("https://")) {
    return std::unexpected(config_error("provider endpoint base_url must use http or https", profile.target)
                               .with("field", "base_url")
                               .with("base_url", profile.base_url));
  }
  if (profile.target.protocol != ProtocolKind::anthropic_messages &&
      profile.target.protocol != ProtocolKind::openai_responses) {
    return std::unexpected(config_error("provider transport protocol is not implemented", profile.target));
  }
  return {};
}

[[nodiscard]] core::Result<std::string> resolve_api_key(const ResolvedProfileTarget& profile,
                                                        const SecretLookup& secrets) {
  auto failure = [&](std::string message) {
    return Error{core::ErrorKind::auth, std::move(message)}
        .with("profile", profile.target.profile)
        .with("api_key_env", profile.api_key_env);
  };

  if (secrets) {
    try {
      auto key = secrets(profile.api_key_env);
      if (!key || key->empty()) {
        return std::unexpected(failure("provider credential is unavailable"));
      }
      return std::move(*key);
    } catch (...) {
      return std::unexpected(failure("provider credential lookup failed"));
    }
  }

  const auto* value = std::getenv(profile.api_key_env.c_str());
  if (value == nullptr) {
    return std::unexpected(failure("provider API-key environment variable is not set"));
  }
  if (*value == '\0') {
    return std::unexpected(failure("provider API-key environment variable is empty"));
  }
  return std::string{value};
}

[[nodiscard]] std::string join_url(std::string_view base_url, std::string_view path) {
  auto url = std::string{base_url};
  if (url.ends_with('/') && path.starts_with('/')) {
    url.pop_back();
  } else if (!url.ends_with('/') && !path.starts_with('/')) {
    url.push_back('/');
  }
  url.append(path);
  return url;
}

[[nodiscard]] ProtocolHttpHeader header(std::string name, std::string value) {
  return ProtocolHttpHeader{.name = std::move(name), .value = std::move(value)};
}

[[nodiscard]] std::vector<ProtocolHttpHeader> protocol_headers(const Endpoint& endpoint) {
  auto headers = std::vector<ProtocolHttpHeader>{
      header("content-type", "application/json"),
      header("accept", "application/json"),
  };

  switch (endpoint.target.protocol) {
    case ProtocolKind::anthropic_messages:
      headers.push_back(header("x-api-key", endpoint.api_key));
      headers.push_back(header("anthropic-version", "2023-06-01"));
      break;
    case ProtocolKind::openai_responses:
      headers.push_back(header("authorization", "Bearer " + endpoint.api_key));
      break;
    case ProtocolKind::openai_chat_completions:
    case ProtocolKind::gemini_generate_content:
    case ProtocolKind::custom_openai_compatible:
      break;
  }

  return headers;
}

[[nodiscard]] Error http_status_error(const ProtocolHttpResponse& response, const ModelTarget& target) {
  auto with_status = [&](Error error) {
    return std::move(error)
        .with("provider_profile", target.profile)
        .with("provider_model", target.model)
        .with("protocol", std::string{core::enum_name(target.protocol)})
        .with("http_status", std::to_string(response.status_code))
        .with("body_bytes", std::to_string(response.body_json.size()));
  };

  if (response.status_code == 401 || response.status_code == 403) {
    return with_status(Error{core::ErrorKind::auth, "provider authentication failed"});
  }
  if (response.status_code == 408) {
    return with_status(Error{core::ErrorKind::timeout, "provider request timed out"});
  }
  if (response.status_code == 429) {
    return with_status(Error::rate_limit("provider rate limited"));
  }
  if (response.status_code >= 500) {
    return with_status(Error::upstream("provider upstream error"));
  }
  if (response.status_code >= 400) {
    return with_status(Error::invalid_argument("provider rejected request"));
  }
  return with_status(Error::network("provider transport returned non-success status"));
}

class ProtocolTransportSystem final : public System {
public:
  ProtocolTransportSystem(ProtocolTransport& transport, std::vector<Endpoint> endpoints)
      : transport_{&transport}, endpoints_{std::move(endpoints)} {}

  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send(Request request, ModelTarget target, EventSink* sink = nullptr) const override {
    const auto match =
        std::ranges::find(endpoints_, target.profile, [](const Endpoint& endpoint) -> const std::string& {
          return endpoint.target.profile;
        });
    if (match == endpoints_.end()) {
      co_return std::unexpected(config_error("provider endpoint not available for route profile", target));
    }
    const auto& endpoint = *match;
    if (target.model != endpoint.target.model) {
      co_return std::unexpected(
          config_error("provider endpoint route model mismatch", endpoint.target).with("route_model", target.model));
    }
    if (target.protocol != endpoint.target.protocol) {
      co_return std::unexpected(config_error("provider endpoint route protocol mismatch", endpoint.target)
                                    .with("route_protocol", std::string{core::enum_name(target.protocol)}));
    }

    if (request.stream && transport_->supports_streaming()) {
      co_return co_await send_stream(std::move(request), std::move(target), endpoint, sink);
    }

    request.stream = false;
    auto protocol = make_protocol_request(request, target);
    if (!protocol) {
      co_return std::unexpected(std::move(protocol).error());
    }

    auto http_response = co_await transport_->send(ProtocolHttpRequest{
        .method = protocol->method,
        .url = join_url(endpoint.base_url, protocol->path),
        .headers = protocol_headers(endpoint),
        .body_json = std::move(protocol->body_json),
    });
    if (!http_response) {
      co_return std::unexpected(std::move(http_response)
                                    .error()
                                    .with("provider_profile", target.profile)
                                    .with("provider_model", target.model)
                                    .with("protocol", std::string{core::enum_name(target.protocol)}));
    }

    if (http_response->status_code < 200 || http_response->status_code >= 300) {
      co_return std::unexpected(http_status_error(*http_response, target));
    }

    auto decoded = decode_protocol_response(http_response->body_json, target);
    if (!decoded) {
      co_return std::unexpected(
          std::move(decoded).error().with("provider_profile", target.profile).with("provider_model", target.model));
    }
    if (sink != nullptr) {
      sink->on_done(decoded->stop_reason);
    }
    co_return decoded;
  }

private:
  // Transport completion joins every callback borrowing these decoders.
  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send_stream(Request request, ModelTarget target, const Endpoint& endpoint, EventSink* sink) const {
    request.stream = true;
    auto protocol = make_protocol_request(request, target);
    if (!protocol) {
      co_return std::unexpected(std::move(protocol).error());
    }

    const bool openai = target.protocol == ProtocolKind::openai_responses;
    auto anthropic_decoder = std::optional<detail::AnthropicSseDecoder>{};
    auto openai_decoder = std::optional<detail::OpenAiResponsesSseDecoder>{};
    if (openai) {
      openai_decoder.emplace(target, sink);
    } else {
      anthropic_decoder.emplace(target, sink);
    }
    auto http_response = co_await transport_->send_streaming(
        ProtocolHttpRequest{
            .method = protocol->method,
            .url = join_url(endpoint.base_url, protocol->path),
            .headers = protocol_headers(endpoint),
            .body_json = std::move(protocol->body_json),
        },
        [&anthropic_decoder, &openai_decoder](std::string_view event, std::string_view data) {
          if (openai_decoder.has_value()) {
            openai_decoder->consume(event, data);
          } else {
            anthropic_decoder->consume(event, data);
          }
        });
    if (!http_response) {
      co_return std::unexpected(std::move(http_response)
                                    .error()
                                    .with("provider_profile", target.profile)
                                    .with("provider_model", target.model)
                                    .with("protocol", std::string{core::enum_name(target.protocol)}));
    }

    if (http_response->status_code < 200 || http_response->status_code >= 300) {
      co_return std::unexpected(http_status_error(*http_response, target));
    }

    auto assembled = openai ? openai_decoder->result() : anthropic_decoder->result();
    if (!assembled) {
      co_return std::unexpected(
          std::move(assembled).error().with("provider_profile", target.profile).with("provider_model", target.model));
    }
    co_return assembled;
  }

  ProtocolTransport* transport_;
  std::vector<Endpoint> endpoints_;
};

}  // namespace

Route RouteProfileResolution::route() const {
  return {
      .primary = primary.target,
      .fallbacks = fallbacks | std::views::transform(&ResolvedProfileTarget::target) |
                   std::ranges::to<std::vector<ModelTarget>>(),
  };
}

async::Awaitable<core::Result<ProtocolHttpResponse>>
ProtocolTransport::send_streaming(ProtocolHttpRequest request, ProtocolSseCallback on_event) const {
  static_cast<void>(request);
  static_cast<void>(on_event);
  co_return std::unexpected(Error::internal("protocol transport does not implement streaming"));
}

core::Result<std::unique_ptr<System>>
make_protocol_system(ProtocolTransport& transport, RouteProfileResolution resolution, SecretLookup secrets) {
  auto profiles = std::move(resolution.fallbacks);
  profiles.insert(profiles.begin(), std::move(resolution.primary));

  // Preflight the whole route before looking up even the primary credential.
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    const auto& profile = profiles[i];
    const auto* role = i == 0 ? "primary" : "fallback";
    if (auto valid = validate_profile(profile); !valid) {
      return std::unexpected(std::move(valid).error().with("role", role));
    }
    if (std::ranges::contains(
            std::span{profiles}.first(i),
            profile.target.profile,
            [](const ResolvedProfileTarget& previous) -> const std::string& { return previous.target.profile; })) {
      return std::unexpected(
          config_error("provider route contains duplicate profiles", profile.target).with("role", role));
    }
  }

  auto endpoints = std::vector<Endpoint>{};
  endpoints.reserve(profiles.size());
  for (auto& profile : profiles) {
    auto key = resolve_api_key(profile, secrets);
    if (!key) {
      return std::unexpected(std::move(key).error().with("role", endpoints.empty() ? "primary" : "fallback"));
    }
    endpoints.push_back(Endpoint{
        .target = std::move(profile.target),
        .base_url = std::move(profile.base_url),
        .api_key = std::move(*key),
    });
  }
  return std::make_unique<ProtocolTransportSystem>(transport, std::move(endpoints));
}

}  // namespace orangutan::provider
