#include <oran/bootstrap/provider_backend.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <oran/bootstrap/provider-profiles.hpp>
#include <oran/config.hpp>
#include <oran/core/error.hpp>
#include <oran/http.hpp>
#include <oran/provider.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Error option_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

class HttpProtocolTransport final : public provider::ProtocolTransport {
public:
  HttpProtocolTransport(http::Client& client, std::chrono::milliseconds request_timeout, std::uint64_t max_stream_bytes)
      : client_{&client}, request_timeout_{request_timeout}, max_stream_bytes_{max_stream_bytes} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    auto response = co_await client_->send(to_body_request(std::move(request)));
    if (!response) {
      co_return std::unexpected(std::move(response).error().with("transport", "oran-http"));
    }
    co_return to_protocol_response(std::move(*response));
  }

  [[nodiscard]] bool supports_streaming() const noexcept override {
    return true;
  }

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send_streaming(provider::ProtocolHttpRequest request, provider::ProtocolSseCallback on_event) const override {
    auto response =
        co_await client_->send_streaming(to_body_request(std::move(request)), [&on_event](const http::SseEvent& event) {
          on_event(event.event, event.data);
        });
    if (!response) {
      co_return std::unexpected(std::move(response).error().with("transport", "oran-http"));
    }
    co_return to_protocol_response(std::move(*response));
  }

private:
  [[nodiscard]] http::BodyRequest to_body_request(provider::ProtocolHttpRequest request) const {
    auto headers = std::vector<http::Header>{};
    headers.reserve(request.headers.size());
    for (auto& header : request.headers) {
      headers.push_back(http::Header{.name = std::move(header.name), .value = std::move(header.value)});
    }
    return http::BodyRequest{
        .method = std::move(request.method),
        .url = std::move(request.url),
        .headers = std::move(headers),
        .body = std::move(request.body_json),
        .timeout = request_timeout_,
        .max_bytes = max_stream_bytes_,
    };
  }

  [[nodiscard]] static provider::ProtocolHttpResponse to_protocol_response(http::BodyResponse response) {
    auto headers = std::vector<provider::ProtocolHttpHeader>{};
    headers.reserve(response.headers.size());
    for (auto& header : response.headers) {
      headers.push_back(provider::ProtocolHttpHeader{.name = std::move(header.name), .value = std::move(header.value)});
    }
    return provider::ProtocolHttpResponse{
        .status_code = response.status_code,
        .headers = std::move(headers),
        .body_json = std::move(response.body),
    };
  }

  http::Client* client_;
  std::chrono::milliseconds request_timeout_;
  std::uint64_t max_stream_bytes_;
};

}  // namespace

struct HttpProviderBackend::Impl {
  provider::Route route;
  http::Client client;
  HttpProtocolTransport transport;
  std::unique_ptr<provider::System> system;

  Impl(provider::Route route_value,
       asio::any_io_executor blocking_executor,
       std::chrono::milliseconds request_timeout,
       std::uint64_t max_stream_bytes)
      : route{std::move(route_value)}, client{std::move(blocking_executor)},
        transport{client, request_timeout, max_stream_bytes} {}
};

HttpProviderBackend::HttpProviderBackend(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

HttpProviderBackend::HttpProviderBackend(HttpProviderBackend&&) noexcept = default;

HttpProviderBackend& HttpProviderBackend::operator=(HttpProviderBackend&&) noexcept = default;

HttpProviderBackend::~HttpProviderBackend() = default;

provider::System& HttpProviderBackend::system() noexcept {
  return *impl_->system;
}

const provider::System& HttpProviderBackend::system() const noexcept {
  return *impl_->system;
}

const provider::Route& HttpProviderBackend::route() const noexcept {
  return impl_->route;
}

core::Result<HttpProviderBackend> HttpProviderBackend::build(const config::Config& config,
                                                             HttpProviderBackendOptions options) {
  if (!options.blocking_executor) {
    return std::unexpected(option_error("HTTP provider backend requires a blocking executor"));
  }
  if (options.request_timeout.count() <= 0) {
    return std::unexpected(option_error("HTTP provider backend request timeout must be positive"));
  }
  if (options.max_stream_bytes == 0) {
    return std::unexpected(option_error("HTTP provider backend max_stream_bytes must be positive"));
  }
  if (options.route_name.empty()) {
    return std::unexpected(option_error("HTTP provider backend route name must be non-empty"));
  }

  auto resolution = resolve_route_profiles(config, options.route_name);
  if (!resolution) {
    return std::unexpected(std::move(resolution).error());
  }
  auto impl = std::make_unique<Impl>(resolution->route(),
                                     std::move(options.blocking_executor),
                                     options.request_timeout,
                                     options.max_stream_bytes);
  auto system = provider::make_protocol_system(impl->transport, std::move(*resolution), std::move(options.secrets));
  if (!system) {
    return std::unexpected(std::move(system).error());
  }
  impl->system = std::move(*system);

  return HttpProviderBackend{std::move(impl)};
}

}  // namespace orangutan::bootstrap
