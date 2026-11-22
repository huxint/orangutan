// tests/http/test_streaming.cpp - SSE streaming client coverage.

#include <oran/http.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace test = orangutan::tests;
using asio::ip::tcp;

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

// A localhost server that writes one canned response. When `keep_open` is set
// it holds the connection until the client disconnects, so a streaming caller
// stays mid-stream (used by the cancellation case); otherwise it closes after
// writing, which terminates the stream for the happy/error cases.
class SseTestServer {
public:
  explicit SseTestServer(std::string response, bool keep_open = false)
      : response_{std::move(response)}, keep_open_{keep_open} {
    tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
  }

  ~SseTestServer() {
    std::error_code ignored;
    if (!served_.load() && port_ != 0) {
      asio::io_context poke_io;
      tcp::socket poke{poke_io};
      poke.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port_}, ignored);
      poke.close(ignored);
    }
    acceptor_.close(ignored);
  }

  [[nodiscard]] std::string url(std::string_view path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
  }

  [[nodiscard]] bool served() const noexcept {
    return served_.load();
  }

  SseTestServer(const SseTestServer&) = delete;
  SseTestServer& operator=(const SseTestServer&) = delete;

private:
  void serve() {
    std::error_code ec;
    auto socket = acceptor_.accept(ec);
    if (ec) {
      return;
    }

    std::array<char, 4096> buffer{};
    std::string request;
    for (;;) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (read > 0) {
        request.append(buffer.data(), read);
      }
      if (request.contains("\r\n\r\n")) {
        break;
      }
      if (ec) {
        return;
      }
    }

    asio::write(socket, asio::buffer(response_), ec);
    served_ = !ec;

    if (keep_open_) {
      while (!ec) {
        socket.read_some(asio::buffer(buffer), ec);  // Drain until the client disconnects.
      }
    }

    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::string response_;
  bool keep_open_;
  std::atomic_bool served_{false};
  std::jthread worker_;
};

}  // namespace

TEST_CASE("Client streams SSE events and resolves with an empty body", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  SseTestServer server{"HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "event: message_start\r\ndata: {\"a\":1}\r\n\r\n"
                       "data: hello\r\n\r\n"
                       "event: message_stop\r\ndata: {}\r\n\r\n"};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  std::vector<http::SseEvent> events;
  std::thread::id callback_thread;
  const auto main_thread = std::this_thread::get_id();

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.body = "{}";
    request.timeout = 2s;

    auto response = co_await client.send_streaming(std::move(request), [&](const http::SseEvent& event) {
      callback_thread = std::this_thread::get_id();
      events.push_back(event);
    });

    REQUIRE(response.has_value());
    REQUIRE(response->status_code == 200);
    REQUIRE(response->body.empty());
  });

  REQUIRE(events.size() == 3);
  REQUIRE(events[0] == http::SseEvent{.event = "message_start", .data = R"({"a":1})"});
  REQUIRE(events[1] == http::SseEvent{.event = "message", .data = "hello"});
  REQUIRE(events[2] == http::SseEvent{.event = "message_stop", .data = "{}"});
  REQUIRE(callback_thread == main_thread);
  blocking.join();
}

TEST_CASE("Client streaming serializes event callbacks on a multi-worker executor", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};

  std::string body = "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Connection: close\r\n"
                     "\r\n";
  for (int i = 0; i < 32; ++i) {
    body.append("data: ").append(std::to_string(i)).append("\r\n\r\n");
  }

  SseTestServer server{std::move(body)};
  asio::io_context io{2};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  std::atomic_int active_callbacks{0};
  std::atomic_int max_active_callbacks{0};
  std::mutex events_mutex;
  std::vector<std::string> events;
  std::optional<core::Result<http::BodyResponse>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<http::BodyResponse>> {
        auto request = http::BodyRequest{};
        request.method = "POST";
        request.url = server.url("/v1/messages");
        request.body = "{}";
        request.timeout = 2s;

        co_return co_await client.send_streaming(std::move(request), [&](const http::SseEvent& event) {
          const auto active = active_callbacks.fetch_add(1) + 1;
          auto observed = max_active_callbacks.load();
          while (active > observed && !max_active_callbacks.compare_exchange_weak(observed, active)) {}
          std::this_thread::sleep_for(1ms);
          {
            const std::scoped_lock lock{events_mutex};
            events.push_back(event.data);
          }
          active_callbacks.fetch_sub(1);
        });
      },
      [&](std::exception_ptr ep, core::Result<http::BodyResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      });

  std::jthread first{[&] { io.run(); }};
  std::jthread second{[&] { io.run(); }};
  first.join();
  second.join();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  REQUIRE((*result)->status_code == 200);
  REQUIRE(max_active_callbacks.load() == 1);
  REQUIRE(events.size() == 32);
  for (int i = 0; i < 32; ++i) {
    REQUIRE(events[static_cast<std::size_t>(i)] == std::to_string(i));
  }
  blocking.join();
}

TEST_CASE("Client streaming returns the body for a non-stream error response", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  SseTestServer server{"HTTP/1.1 401 Unauthorized\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       R"({"error":"unauthorized"})"};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  std::vector<http::SseEvent> events;

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.body = "{}";
    request.timeout = 2s;

    auto response = co_await client.send_streaming(std::move(request),
                                                   [&](const http::SseEvent& event) { events.push_back(event); });

    REQUIRE(response.has_value());
    REQUIRE(response->status_code == 401);
    REQUIRE(response->body == R"({"error":"unauthorized"})");
  });

  REQUIRE(events.empty());
  blocking.join();
}

TEST_CASE("Client streaming aborts a response that exceeds max_bytes", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  // A server that floods far more SSE bytes than the client budget allows.
  constexpr std::size_t kStreamBytes = 256 * 1024;
  std::string body;
  body.reserve(kStreamBytes + 16);
  for (std::size_t i = 0; i < kStreamBytes / 16; ++i) {
    body.append("data: 0123456789abcdef\n\n");
  }
  SseTestServer server{"HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "Connection: close\r\n"
                       "\r\n" +
                       body};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.body = "{}";
    request.timeout = 2s;
    request.max_bytes = 1024;

    std::size_t events = 0;
    auto response =
        co_await client.send_streaming(std::move(request), [&](const http::SseEvent& /*event*/) { ++events; });

    // The transfer is aborted at the budget, so the caller gets an error —
    // never a success with a truncated event stream — and any events decoded
    // before the abort were already delivered.
    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::network);
    REQUIRE(response.error().message().contains("max_bytes"));
  });

  blocking.join();
}

TEST_CASE("Client streaming returns an error when the error body exceeds max_bytes", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  std::string error_body;
  error_body.assign(64 * 1024, 'x');
  SseTestServer server{"HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "\r\n" +
                       error_body};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.body = "{}";
    request.timeout = 2s;
    request.max_bytes = 1024;

    auto response = co_await client.send_streaming(std::move(request), [](const http::SseEvent& /*event*/) {});

    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::network);
    REQUIRE(response.error().message().contains("max_bytes"));
  });

  blocking.join();
}

TEST_CASE("Client streaming observes mid-stream cancellation", "[unit][http][streaming]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  SseTestServer server{"HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "\r\n"
                       "data: one\r\n\r\n",
                       /*keep_open=*/true};
  asio::io_context io;
  asio::thread_pool blocking{1};
  asio::cancellation_signal signal;
  auto client = http::Client{blocking.get_executor()};
  std::optional<core::Result<http::BodyResponse>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<http::BodyResponse>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        auto request = http::BodyRequest{};
        request.method = "POST";
        request.url = server.url("/v1/messages");
        request.body = "{}";
        request.timeout = 10s;
        co_return co_await client.send_streaming(std::move(request), [](const http::SseEvent&) {});
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<http::BodyResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::steady_timer fire{io};
  fire.expires_after(150ms);
  fire.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  blocking.join();
}
