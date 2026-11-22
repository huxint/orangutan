#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::provider {

using SecretLookup = std::function<core::Result<std::string>(std::string_view)>;

/// Model policy and endpoint metadata supplied by the host. `api_key_env` names
/// a credential; it contains no secret value.
struct ResolvedProfileTarget {
  ModelTarget target;
  std::string base_url;
  std::string api_key_env;

  friend bool operator==(const ResolvedProfileTarget&, const ResolvedProfileTarget&) = default;
};

struct RouteProfileResolution {
  ResolvedProfileTarget primary;
  std::vector<ResolvedProfileTarget> fallbacks;

  [[nodiscard]] Route route() const;

  friend bool operator==(const RouteProfileResolution&, const RouteProfileResolution&) = default;
};

struct ProtocolHttpHeader {
  std::string name;
  std::string value;

  friend bool operator==(const ProtocolHttpHeader&, const ProtocolHttpHeader&) = default;
};

struct ProtocolHttpRequest {
  std::string method;
  std::string url;
  std::vector<ProtocolHttpHeader> headers;
  std::string body_json;

  friend bool operator==(const ProtocolHttpRequest&, const ProtocolHttpRequest&) = default;
};

struct ProtocolHttpResponse {
  std::uint16_t status_code{0};
  std::vector<ProtocolHttpHeader> headers;
  std::string body_json;

  friend bool operator==(const ProtocolHttpResponse&, const ProtocolHttpResponse&) = default;
};

/// Invoked once per decoded SSE event during a streaming send. Provider-owned
/// (rather than `http::SseEvent`) so `oran-http` types stay off the provider's
/// public surface, symmetric with `ProtocolHttpRequest`/`ProtocolHttpResponse`
/// forming the transport seam instead of `http::BodyRequest`. `event` defaults
/// to "message" per the SSE grammar; `data` is the joined `data:` payload.
using ProtocolSseCallback = std::function<void(std::string_view event, std::string_view data)>;

/// Minimal async transport consumed by protocol adapters.
///
/// Implementations own actual HTTP, TLS, and SSE mechanics. Requests may carry
/// secret authorization headers; implementations must not log them.
class ProtocolTransport {
public:
  ProtocolTransport() = default;
  virtual ~ProtocolTransport() = default;

  ProtocolTransport(const ProtocolTransport&) = delete;
  ProtocolTransport& operator=(const ProtocolTransport&) = delete;
  ProtocolTransport(ProtocolTransport&&) = delete;
  ProtocolTransport& operator=(ProtocolTransport&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<ProtocolHttpResponse>>
  send(ProtocolHttpRequest request) const = 0;

  /// Whether this transport implements `send_streaming`. Adapters consult this
  /// before choosing the streaming path so a transport that only does body
  /// requests (or has not yet implemented streaming) stays on the body path.
  /// Defaults to `false`; streaming-capable transports override it.
  [[nodiscard]] virtual bool supports_streaming() const noexcept {
    return false;
  }

  /// Send one request and stream the response, invoking `on_event` once per
  /// decoded SSE event on the caller's executor. On a 2xx event stream the
  /// resolved `ProtocolHttpResponse` carries the status and headers with an
  /// empty body; on any other response no events fire and the full body is
  /// returned for the caller to decode. The default returns an error so a
  /// transport that reports `supports_streaming() == false` is never expected
  /// to stream.
  [[nodiscard]] virtual async::Awaitable<core::Result<ProtocolHttpResponse>>
  send_streaming(ProtocolHttpRequest request, ProtocolSseCallback on_event) const;
};

/// Validate every endpoint and unique profile before reading credentials from
/// the supplied lookup or named environment variables. The returned system owns
/// its credentials; transport must outlive the system and every awaited send.
///
/// Each send accepts one selected profile/model/protocol. `execution::run`
/// owns retry/fallback selection. Streaming requires both `Request::stream` and
/// transport support. Credential values never enter error diagnostics.
[[nodiscard]] core::Result<std::unique_ptr<System>> make_protocol_system(ProtocolTransport& transport,
                                                                         RouteProfileResolution resolution,
                                                                         SecretLookup secrets = {});

}  // namespace orangutan::provider
