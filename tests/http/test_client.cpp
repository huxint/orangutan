// tests/http/test_client.cpp - libcurl body client coverage.

#include <oran/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/ip/tcp.hpp>
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

class OneShotHttpServer {
public:
  explicit OneShotHttpServer(std::string response, bool stall_response = false)
      : response_{std::move(response)}, stall_response_{stall_response} {
    tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this](std::stop_token stop) { serve(stop); }};
  }

  ~OneShotHttpServer() {
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

  [[nodiscard]] std::string request_text() const {
    return request_;
  }

  [[nodiscard]] bool served() const noexcept {
    return served_.load();
  }

  [[nodiscard]] bool request_received() const noexcept {
    return request_received_.load(std::memory_order_acquire);
  }

private:
  void serve(std::stop_token stop) {
    std::error_code ec;
    auto socket = acceptor_.accept(ec);
    if (ec) {
      return;
    }

    std::array<char, 4096> buffer{};
    for (;;) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (ec && ec != asio::error::eof) {
        return;
      }
      if (read > 0) {
        request_.append(buffer.data(), read);
      }
      if (request_.contains("\r\n\r\n")) {
        if (!request_.contains("Content-Length:") || request_.ends_with("hello")) {
          break;
        }
      }
      if (ec == asio::error::eof) {
        break;
      }
    }

    request_received_.store(true, std::memory_order_release);
    while (stall_response_ && !stop.stop_requested()) {
      std::this_thread::sleep_for(5ms);
    }
    if (stop.stop_requested()) {
      return;
    }

    asio::write(socket, asio::buffer(response_), ec);
    served_ = !ec;
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::string response_;
  bool stall_response_{false};
  std::string request_;
  std::atomic_bool served_{false};
  std::atomic_bool request_received_{false};
  std::jthread worker_;
};

}  // namespace

TEST_CASE("Client sends a body request and collects the response", "[unit][http][client]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  OneShotHttpServer server{"HTTP/1.1 201 Created\r\n"
                           "Content-Type: application/json\r\n"
                           "X-Trace: abc\r\n"
                           "Content-Length: 11\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "{\"ok\":true}"};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.headers.push_back(http::Header{.name = "Authorization", .value = "Bearer test"});
    request.body = "hello";
    request.timeout = 2s;

    auto response = co_await client.send(std::move(request));

    REQUIRE(response.has_value());
    REQUIRE(response->status_code == 201);
    REQUIRE(response->body == R"({"ok":true})");
    REQUIRE(response->headers.size() >= 2);
  });

  REQUIRE(server.served());
  REQUIRE(server.request_text().starts_with("POST /v1/messages HTTP/1.1"));
  REQUIRE(server.request_text().contains("Authorization: Bearer test"));
  REQUIRE(server.request_text().contains("hello"));
  blocking.join();
}

TEST_CASE("Client validates request shape before curl dispatch", "[unit][http][client]") {
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  SECTION("missing url") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "GET",
          .url = "",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("unsupported scheme") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "GET",
          .url = "file:///tmp/nope",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("unsupported method") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "TRACE",
          .url = "http://127.0.0.1/",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  blocking.join();
}

TEST_CASE("Client observes cancellation before dispatch", "[unit][http][client]") {
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
        co_return co_await client.send(http::BodyRequest{
            .method = "GET",
            .url = "http://127.0.0.1/",
            .headers = {},
            .body = {},
            .timeout = 10s,
        });
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<http::BodyResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  blocking.join();
}

TEST_CASE("Client bridges in-flight cancellation to the blocking transport thread", "[unit][http][client]") {
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  OneShotHttpServer server{"", true};
  asio::io_context io;
  asio::thread_pool blocking{1};
  asio::cancellation_signal signal;
  auto client = http::Client{blocking.get_executor()};
  auto result = std::optional<core::Result<http::BodyResponse>>{};
  auto failure = std::exception_ptr{};
  const auto started_at = std::chrono::steady_clock::now();

  asio::co_spawn(
      io,
      client.send(http::BodyRequest{
          .method = "GET",
          .url = server.url("/stalled"),
          .headers = {},
          .body = {},
          .timeout = 10s,
      }),
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<http::BodyResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  auto cancellation_poll = asio::steady_timer{io};
  cancellation_poll.expires_after(5ms);
  std::function<void(const std::error_code&)> cancel_when_started;
  cancel_when_started = [&](const std::error_code& ec) {
    if (ec) {
      return;
    }
    if (server.request_received()) {
      signal.emit(asio::cancellation_type::terminal);
      return;
    }
    cancellation_poll.expires_after(5ms);
    cancellation_poll.async_wait(cancel_when_started);
  };
  cancellation_poll.async_wait(cancel_when_started);
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(std::chrono::steady_clock::now() - started_at < 1s);
  blocking.join();
}
