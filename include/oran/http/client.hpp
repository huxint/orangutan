#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::http {

struct Header {
  std::string name;
  std::string value;

  friend bool operator==(const Header&, const Header&) = default;
};

struct BodyRequest {
  std::string method{"GET"};
  std::string url;
  std::vector<Header> headers;
  std::string body;
  std::chrono::milliseconds timeout{30000};
  /// Response-body byte cap, including SSE and error bodies. Exceeding it
  /// aborts the transfer with a network error carrying `max_bytes`.
  std::uint64_t max_bytes{16 * 1024 * 1024};

  friend bool operator==(const BodyRequest&, const BodyRequest&) = default;
};

struct BodyResponse {
  std::uint16_t status_code{0};
  std::vector<Header> headers;
  std::string body;

  friend bool operator==(const BodyResponse&, const BodyResponse&) = default;
};

/// One decoded Server-Sent Events message. `event` defaults to "message" per
/// the SSE grammar; `data` is the `data:` payload, multi-line values joined
/// with "\n".
struct SseEvent {
  std::string event{"message"};
  std::string data;

  friend bool operator==(const SseEvent&, const SseEvent&) = default;
};

/// Invoked once per decoded SSE event during a streaming send. In production
/// the callback runs on the caller's coroutine executor, never on the blocking
/// transport thread.
using SseEventCallback = std::function<void(const SseEvent&)>;

class Client {
public:
  /// The caller-owned executor is where libcurl's blocking transport work
  /// runs. In production this should be `async::Runtime::cpu_executor()`.
  explicit Client(asio::any_io_executor blocking_executor);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  /// Send one non-streaming request and collect the complete response body.
  [[nodiscard]] async::Awaitable<core::Result<BodyResponse>> send(BodyRequest request) const;

  /// Send one request and stream the response. On a 2xx `text/event-stream`
  /// response, each decoded `SseEvent` is delivered to `on_event` and the
  /// resolved `BodyResponse` carries the status and headers with an empty body.
  /// On any other response, no events fire and the full body is returned for
  /// the caller to decode (e.g. an error payload). Event callbacks finish before
  /// this operation returns.
  [[nodiscard]] async::Awaitable<core::Result<BodyResponse>> send_streaming(BodyRequest request,
                                                                            SseEventCallback on_event) const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace orangutan::http
