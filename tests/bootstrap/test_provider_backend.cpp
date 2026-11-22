#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
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

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(std::string name) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
      unsetenv(name_.c_str());
    }
  }

  ~ScopedUnsetEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class OneShotHttpServer {
public:
  explicit OneShotHttpServer(std::string response) : response_{std::move(response)} {
    tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
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

  [[nodiscard]] std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  [[nodiscard]] std::string request_text() const {
    return request_;
  }

  [[nodiscard]] bool served() const noexcept {
    return served_.load();
  }

private:
  [[nodiscard]] static std::optional<std::size_t> content_length(std::string_view request) {
    constexpr auto kHeader = std::string_view{"Content-Length:"};
    const auto header = request.find(kHeader);
    if (header == std::string_view::npos) {
      return std::nullopt;
    }
    auto value = request.substr(header + kHeader.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    const auto end = value.find("\r\n");
    if (end != std::string_view::npos) {
      value = value.substr(0, end);
    }

    auto length = std::size_t{0};
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto parsed = std::from_chars(first, last, length);
    if (parsed.ec != std::errc{} || parsed.ptr == first) {
      return std::nullopt;
    }
    return length;
  }

  [[nodiscard]] static bool request_complete(std::string_view request) {
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
      return false;
    }
    const auto length = content_length(request);
    if (!length) {
      return true;
    }
    return request.size() >= header_end + 4 + *length;
  }

  void serve() {
    std::error_code ec;
    auto socket = acceptor_.accept(ec);
    if (ec) {
      return;
    }

    std::array<char, 4096> buffer{};
    while (!request_complete(request_)) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (ec && ec != asio::error::eof) {
        return;
      }
      if (read > 0) {
        request_.append(buffer.data(), read);
      }
      if (ec == asio::error::eof) {
        break;
      }
    }

    asio::write(socket, asio::buffer(response_), ec);
    served_ = !ec;
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::string response_;
  std::string request_;
  std::atomic_bool served_{false};
  std::jthread worker_;
};

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string config_text(std::string base_url) {
  auto text = std::string{R"json(
{
  "profiles": {
    "default": {
      "provider": "anthropic",
      "protocol": "anthropic_messages",
      "model": "claude-test",
      "base_url": ")json"};
  text.append(base_url);
  text.append(R"json(",
      "api_key_env": "ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": []
    }
  }
}
)json");

  return text;
}

config::Config parse_config(std::string base_url) {
  auto parsed = config::Config::parse(config_text(std::move(base_url)));
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

provider::Request request() {
  auto value = provider::Request{};
  value.messages.push_back(core::Message{
      .role = core::Role::user,
      .blocks = {core::TextContent{.text = "hello"}},
      .created_at = {},
  });
  value.max_tokens = 32;
  value.stream = true;
  return value;
}

std::string anthropic_response() {
  constexpr auto kBody = std::string_view{
      R"json({"type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"backend ok"}],"stop_reason":"end_turn","usage":{"input_tokens":4,"output_tokens":2}})json"};
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: " +
         std::to_string(kBody.size()) +
         "\r\n"
         "Connection: close\r\n"
         "\r\n" +
         std::string{kBody};
}

// A canned Anthropic Messages SSE stream that assembles to the same logical
// turn as `anthropic_response()`. Mirrors the wire grammar the oran-http SSE
// parser feeds the provider's `AnthropicSseDecoder`.
std::string anthropic_sse_response() {
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: text/event-stream\r\n"
         "Connection: close\r\n"
         "\r\n"
         "event: message_start\r\n"
         R"(data: {"type":"message_start","message":{"id":"msg_1","type":"message","role":"assistant",)"
         R"("model":"claude-test","content":[],"stop_reason":null,"usage":{"input_tokens":4,"output_tokens":1}}})"
         "\r\n\r\n"
         "event: content_block_start\r\n"
         R"(data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
         "\r\n\r\n"
         "event: content_block_delta\r\n"
         R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"backend ok"}})"
         "\r\n\r\n"
         "event: content_block_stop\r\n"
         R"(data: {"type":"content_block_stop","index":0})"
         "\r\n\r\n"
         "event: message_delta\r\n"
         R"(data: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":2}})"
         "\r\n\r\n"
         "event: message_stop\r\n"
         R"(data: {"type":"message_stop"})"
         "\r\n\r\n";
}

// Records the ordered streaming callbacks so the round-trip test can assert the
// backend drove the caller's `EventSink`, not just the assembled `Response`.
class CapturingSink final : public provider::EventSink {
public:
  void on_text_delta(std::string_view delta) override {
    text.append(delta);
    ++text_deltas;
  }
  void on_done(core::StopReason stop_reason) override {
    done = stop_reason;
  }

  std::string text;
  std::size_t text_deltas{0};
  std::optional<core::StopReason> done;
};

}  // namespace

TEST_CASE("AgentSession resumes persisted context through the HTTP provider boundary",
          "[integration][bootstrap][continuation]") {
  ScopedEnv api_key{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY", "test-secret"};
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  struct Workspace {
    std::filesystem::path path;
    ~Workspace() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };
  auto id = core::generate_turn_id();
  REQUIRE(id);
  Workspace workspace{std::filesystem::temp_directory_path() / ("oran-session-http-" + core::format_turn_id_hex(*id))};
  REQUIRE(std::filesystem::create_directory(workspace.path));

  for (const auto* prompt : {"Remember the project name.", "Continue the previous turn."}) {
    OneShotHttpServer server{anthropic_sse_response()};
    auto cfg = parse_config(server.base_url());
    asio::thread_pool blocking{1};
    auto backend =
        bootstrap::HttpProviderBackend::build(cfg,
                                              {.blocking_executor = blocking.get_executor(), .request_timeout = 2s});
    REQUIRE(backend.has_value());
    auto assembly = bootstrap::RuntimeAssembly::build(workspace.path.string(), blocking.get_executor());
    REQUIRE(assembly.has_value());

    test::run_async(
        [&](asio::io_context& io) -> async::Awaitable<void> {
          auto caller = asio::make_strand(io);
          auto session = bootstrap::AgentSession::create({
              .executor = caller,
              .blocking_executor = blocking.get_executor(),
              .assembly = &*assembly,
              .config = &cfg,
              .provider = &backend->system(),
              .route = backend->route(),
              .scope_key = workspace.path.string(),
              .session_id = *id,
          });
          REQUIRE(session.has_value());

          auto result =
              co_await asio::co_spawn(caller, (*session)->run_prompt({.prompt = prompt}), asio::use_awaitable);

          INFO((result ? "" : result.error().message()));
          REQUIRE(result.has_value());
          CHECK(result->text == "backend ok");
        },
        5s);

    REQUIRE(server.served());
    const auto request = server.request_text();
    const auto body = nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4));
    const auto& messages = body.at("messages");
    if (std::string_view{prompt} == "Continue the previous turn.") {
      REQUIRE(messages.size() == 3);
      CHECK(messages[0]["role"] == "user");
      CHECK(messages[0]["content"][0]["text"] == "Remember the project name.");
      CHECK(messages[1]["role"] == "assistant");
      CHECK(messages[1]["content"][0]["text"] == "backend ok");
      CHECK(messages[2]["content"][0]["text"] == "Continue the previous turn.");
    }
  }
}

TEST_CASE("HttpProviderBackend constructs an HTTP-backed provider system", "[unit][bootstrap][provider_backend]") {
  ScopedUnsetEnv api_key{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY"};
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  OneShotHttpServer server{anthropic_response()};
  auto cfg = parse_config(server.base_url());
  asio::thread_pool blocking{1};
  std::vector<std::string> lookups;

  auto backend =
      bootstrap::HttpProviderBackend::build(cfg,
                                            bootstrap::HttpProviderBackendOptions{
                                                .blocking_executor = blocking.get_executor(),
                                                .request_timeout = 2s,
                                                .route_name = "default",
                                                .secrets = [&](std::string_view key) -> core::Result<std::string> {
                                                  lookups.emplace_back(key);
                                                  return "test-secret";
                                                },
                                            });

  REQUIRE(backend.has_value());
  REQUIRE(backend->route().primary.profile == "default");
  REQUIRE(backend->route().primary.model == "claude-test");
  REQUIRE(lookups == std::vector<std::string>{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY"});

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    // A non-streaming request stays on the body path even though the transport
    // now advertises streaming support.
    auto req = request();
    req.stream = false;
    auto response = co_await backend->system().send(std::move(req), backend->route().primary, nullptr);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "backend ok");
    REQUIRE(response->usage.input_tokens == 4);
    co_return;
  });

  REQUIRE(server.served());
  REQUIRE(server.request_text().starts_with("POST /v1/messages HTTP/1.1"));
  REQUIRE(server.request_text().contains("x-api-key: test-secret"));
  REQUIRE(server.request_text().contains(R"json("model":"claude-test")json"));
  REQUIRE(server.request_text().contains(R"json("stream":false)json"));
  blocking.join();
}

TEST_CASE("HttpProviderBackend streams an Anthropic SSE turn through the EventSink",
          "[unit][bootstrap][provider_backend][streaming]") {
  ScopedEnv api_key{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY", "test-secret"};
  ScopedEnv no_proxy{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv lowercase_no_proxy{"no_proxy", "127.0.0.1,localhost"};
  OneShotHttpServer server{anthropic_sse_response()};
  auto cfg = parse_config(server.base_url());
  asio::thread_pool blocking{1};

  auto backend = bootstrap::HttpProviderBackend::build(cfg,
                                                       bootstrap::HttpProviderBackendOptions{
                                                           .blocking_executor = blocking.get_executor(),
                                                           .request_timeout = 2s,
                                                           .route_name = "default",
                                                       });
  REQUIRE(backend.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await backend->system().send(request(),  // request().stream == true
                                                    backend->route().primary,
                                                    &sink);

    REQUIRE(response.has_value());
    REQUIRE(response->blocks.size() == 1);
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "backend ok");
    REQUIRE(response->stop_reason == core::StopReason::end_turn);
    REQUIRE(response->usage.input_tokens == 4);
    REQUIRE(response->usage.output_tokens == 2);
    co_return;
  });

  REQUIRE(server.served());
  REQUIRE(server.request_text().starts_with("POST /v1/messages HTTP/1.1"));
  REQUIRE(server.request_text().contains(R"json("stream":true)json"));
  REQUIRE(sink.text == "backend ok");
  REQUIRE(sink.text_deltas == 1);
  REQUIRE(sink.done == core::StopReason::end_turn);
  blocking.join();
}

TEST_CASE("HttpProviderBackend reports missing credentials at the construction boundary",
          "[unit][bootstrap][provider_backend]") {
  ScopedUnsetEnv missing{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY"};
  auto cfg = parse_config("http://127.0.0.1:1");
  asio::thread_pool blocking{1};

  auto backend = bootstrap::HttpProviderBackend::build(cfg,
                                                       bootstrap::HttpProviderBackendOptions{
                                                           .blocking_executor = blocking.get_executor(),
                                                           .request_timeout = 2s,
                                                           .route_name = "default",
                                                       });

  REQUIRE_FALSE(backend.has_value());
  REQUIRE(backend.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(backend.error(), "profile") == std::optional<std::string_view>{"default"});
  REQUIRE(context_value(backend.error(), "api_key_env") ==
          std::optional<std::string_view>{"ORAN_BOOTSTRAP_PROVIDER_BACKEND_KEY"});
  blocking.join();
}
