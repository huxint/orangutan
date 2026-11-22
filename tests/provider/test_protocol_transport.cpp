#include <oran/provider.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

using json = ::nlohmann::ordered_json;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

provider::RouteProfileResolution route_profiles(provider::ProtocolKind protocol) {
  const bool anthropic = protocol == provider::ProtocolKind::anthropic_messages;
  return provider::RouteProfileResolution{
      .primary =
          {
              .target = {.profile = anthropic ? "anthropic-main" : "openai-main",
                         .model = anthropic ? "claude-sonnet" : "gpt-main",
                         .protocol = protocol,
                         .thinking_budget = std::nullopt,
                         .cache = std::nullopt},
              .base_url = anthropic ? "https://api.anthropic.com" : "https://api.openai.com/v1/",
              .api_key_env = anthropic ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY",
          },
      .fallbacks = {},
  };
}

core::Result<std::string> test_secret(std::string_view name) {
  if (name == "ANTHROPIC_API_KEY") {
    return "anthropic-secret";
  }
  if (name == "OPENAI_API_KEY") {
    return "openai-secret";
  }
  return std::unexpected(core::Error{core::ErrorKind::auth, "unknown test credential"});
}

provider::Request request() {
  auto request = provider::Request{};
  request.messages.push_back(core::Message{
      .role = core::Role::user,
      .blocks = {core::TextContent{.text = "hello"}},
      .created_at = {},
  });
  request.max_tokens = 64;
  request.stream = true;
  return request;
}

provider::ProtocolHttpResponse anthropic_response() {
  return provider::ProtocolHttpResponse{
      .status_code = 200,
      .headers = {},
      .body_json = R"json({
        "type": "message",
        "role": "assistant",
        "model": "claude-sonnet",
        "content": [{"type": "text", "text": "anthropic ok"}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 4, "output_tokens": 2}
      })json",
  };
}

provider::ProtocolHttpResponse openai_response() {
  return provider::ProtocolHttpResponse{
      .status_code = 200,
      .headers = {},
      .body_json = R"json({
        "status": "completed",
        "model": "gpt-main",
        "output": [
          {
            "type": "message",
            "role": "assistant",
            "content": [{"type": "output_text", "text": "openai ok"}]
          }
        ],
        "usage": {"input_tokens": 5, "output_tokens": 3}
      })json",
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string_view> header_value(const provider::ProtocolHttpRequest& request, std::string_view name) {
  const auto it = std::ranges::find_if(request.headers, [&](const auto& header) { return header.name == name; });
  if (it == request.headers.end()) {
    return std::nullopt;
  }
  return it->value;
}

class RecordingTransport final : public provider::ProtocolTransport {
public:
  explicit RecordingTransport(std::vector<core::Result<provider::ProtocolHttpResponse>> responses)
      : responses_{std::move(responses)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    requests.push_back(std::move(request));
    const auto index = cursor++;
    if (index >= responses_.size()) {
      co_return std::unexpected(core::Error::internal("transport exhausted"));
    }
    const auto& response = responses_[index];
    if (response.has_value()) {
      co_return *response;
    }
    co_return std::unexpected(response.error());
  }

  std::vector<core::Result<provider::ProtocolHttpResponse>> responses_;
  mutable std::vector<provider::ProtocolHttpRequest> requests;
  mutable std::size_t cursor{0};
};

using SseEvent = std::pair<std::string, std::string>;

// A streaming observer that records the ordered delta callbacks so streaming
// tests can assert the System drove the decoder (not just the body path).
class CapturingSink final : public provider::EventSink {
public:
  void on_text_delta(std::string_view delta) override {
    log.push_back("text:" + std::string{delta});
  }
  void on_thinking_delta(std::string_view delta) override {
    log.push_back("thinking:" + std::string{delta});
  }
  void on_tool_start(std::string_view id, std::string_view name) override {
    log.push_back("tool_start:" + std::string{id} + ":" + std::string{name});
  }
  void on_tool_delta(std::string_view id, std::string_view input_delta) override {
    log.push_back("tool_delta:" + std::string{id} + ":" + std::string{input_delta});
  }
  void on_done(core::StopReason stop_reason) override {
    done = stop_reason;
    log.push_back("done");
  }

  std::vector<std::string> log;
  std::optional<core::StopReason> done;
};

// A transport that can replay a canned Anthropic SSE event sequence. It records
// which path the System chose (body vs streaming) so tests can pin the gate.
class StreamingTransport final : public provider::ProtocolTransport {
public:
  [[nodiscard]] bool supports_streaming() const noexcept override {
    return streaming_supported;
  }

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send(provider::ProtocolHttpRequest request) const override {
    body_requests.push_back(std::move(request));
    co_return provider::ProtocolHttpResponse{.status_code = body_status, .headers = {}, .body_json = body_json};
  }

  [[nodiscard]] async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send_streaming(provider::ProtocolHttpRequest request, provider::ProtocolSseCallback on_event) const override {
    streaming_requests.push_back(std::move(request));
    for (const auto& [event, data] : events) {
      on_event(event, data);
    }
    co_return provider::ProtocolHttpResponse{.status_code = stream_status, .headers = {}, .body_json = stream_body};
  }

  std::vector<SseEvent> events;
  bool streaming_supported{true};
  std::uint16_t stream_status{200};
  std::string stream_body;  // empty on a 2xx event stream
  std::uint16_t body_status{200};
  std::string body_json;
  mutable std::vector<provider::ProtocolHttpRequest> body_requests;
  mutable std::vector<provider::ProtocolHttpRequest> streaming_requests;
};

std::vector<SseEvent> text_turn_events() {
  return {
      {"message_start",
       R"({"type":"message_start","message":{"id":"msg_1","type":"message","role":"assistant","model":"claude-sonnet","content":[],"stop_reason":null,"usage":{"input_tokens":4,"output_tokens":1}}})"},
      {"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
      {"content_block_delta",
       R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"anthropic"}})"},
      {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" ok"}})"},
      {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
      {"message_delta",
       R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":2}})"},
      {"message_stop", R"({"type":"message_stop"})"},
  };
}

std::vector<SseEvent> tool_turn_events() {
  return {
      {"message_start",
       R"({"type":"message_start","message":{"id":"msg_2","type":"message","role":"assistant","model":"claude-sonnet","content":[],"stop_reason":null,"usage":{"input_tokens":4,"output_tokens":1}}})"},
      {"content_block_start",
       R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"toolu_1","name":"get_weather","input":{}}})"},
      {"content_block_delta",
       R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"city\":\"NYC\"}"}})"},
      {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
      {"message_delta",
       R"({"type":"message_delta","delta":{"stop_reason":"tool_use","stop_sequence":null},"usage":{"output_tokens":9}})"},
      {"message_stop", R"({"type":"message_stop"})"},
  };
}

std::vector<SseEvent> openai_text_turn_events() {
  return {
      {"response.output_item.added",
       R"({"type":"response.output_item.added","output_index":0,"item":{"id":"msg_1","type":"message","status":"in_progress","role":"assistant","content":[]},"sequence_number":1})"},
      {"response.output_text.delta",
       R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":"openai","sequence_number":2})"},
      {"response.output_text.delta",
       R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":" ok","sequence_number":3})"},
      {"response.completed",
       R"({"type":"response.completed","response":{"id":"resp_1","object":"response","status":"completed","model":"gpt-main","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"openai ok"}]}],"usage":{"input_tokens":5,"output_tokens":3}},"sequence_number":4})"},
  };
}

class PausingTransport final : public provider::ProtocolTransport {
public:
  explicit PausingTransport(asio::any_io_executor executor)
      : started{executor, 2}, primary_release{executor, 1}, fallback_release{executor, 1}, cleanup_started{executor, 1},
        cleanup_release{executor, 1} {}

  bool supports_streaming() const noexcept override {
    return true;
  }

  async::Awaitable<core::Result<provider::ProtocolHttpResponse>> send(provider::ProtocolHttpRequest) const override {
    co_return std::unexpected(core::Error::internal("expected streaming transport"));
  }

  async::Awaitable<core::Result<provider::ProtocolHttpResponse>>
  send_streaming(provider::ProtocolHttpRequest request, provider::ProtocolSseCallback on_event) const override {
    co_await asio::this_coro::throw_if_cancelled(false);
    const bool primary = request.url.ends_with("/messages");
    const auto events = primary ? text_turn_events() : openai_text_turn_events();
    for (const auto& [event, data] : std::span{events}.first(3)) {
      on_event(event, data);
    }
    REQUIRE(started.try_send(std::move(request)).has_value());
    auto released = co_await (primary ? primary_release : fallback_release).receive();
    if (!released) {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
      REQUIRE(cleanup_started.try_send(0).has_value());
      auto cleanup = co_await cleanup_release.receive();
      REQUIRE(cleanup.has_value());
      cleanup_finished = true;
      co_return std::unexpected(core::Error::cancelled());
    }
    for (const auto& [event, data] : std::span{events}.subspan(3)) {
      on_event(event, data);
    }
    co_return provider::ProtocolHttpResponse{.status_code = 200, .headers = {}, .body_json = {}};
  }

  mutable async::Channel<provider::ProtocolHttpRequest> started;
  mutable async::Channel<int> primary_release;
  mutable async::Channel<int> fallback_release;
  mutable async::Channel<int> cleanup_started;
  mutable async::Channel<int> cleanup_release;
  mutable bool cleanup_finished{false};
};

}  // namespace

TEST_CASE("protocol system sends Anthropic Messages bodies", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response()}};
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, nullptr);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "anthropic ok");
    REQUIRE(response->usage.input_tokens == 4);
  });

  REQUIRE(transport.requests.size() == 1);
  const auto& sent = transport.requests.front();
  REQUIRE(sent.method == "POST");
  REQUIRE(sent.url == "https://api.anthropic.com/v1/messages");
  REQUIRE(header_value(sent, "content-type") == std::optional<std::string_view>{"application/json"});
  REQUIRE(header_value(sent, "x-api-key") == std::optional<std::string_view>{"anthropic-secret"});
  REQUIRE(header_value(sent, "anthropic-version") == std::optional<std::string_view>{"2023-06-01"});
  const auto body = json::parse(sent.body_json);
  REQUIRE(body.at("model") == "claude-sonnet");
  REQUIRE(body.at("stream") == false);
}

TEST_CASE("protocol system sends OpenAI Responses bodies", "[unit][provider][protocol]") {
  RecordingTransport transport{{openai_response()}};
  auto profiles = route_profiles(provider::ProtocolKind::openai_responses);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, nullptr);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "openai ok");
    REQUIRE(response->usage.output_tokens == 3);
  });

  REQUIRE(transport.requests.size() == 1);
  const auto& sent = transport.requests.front();
  REQUIRE(sent.method == "POST");
  REQUIRE(sent.url == "https://api.openai.com/v1/responses");
  REQUIRE(header_value(sent, "authorization") == std::optional<std::string_view>{"Bearer openai-secret"});
  const auto body = json::parse(sent.body_json);
  REQUIRE(body.at("model") == "gpt-main");
  REQUIRE(body.at("stream") == false);
}

TEST_CASE("protocol system selects endpoint credentials for each route profile", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response(), openai_response()}};
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  profiles.fallbacks.push_back(route_profiles(provider::ProtocolKind::openai_responses).primary);
  const auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto primary = co_await (*system)->send(request(), route.primary);
    auto fallback = co_await (*system)->send(request(), route.fallbacks.front());

    REQUIRE(primary.has_value());
    REQUIRE(fallback.has_value());
    REQUIRE(std::get<core::TextContent>(primary->blocks.front()).text == "anthropic ok");
    REQUIRE(std::get<core::TextContent>(fallback->blocks.front()).text == "openai ok");
  });

  REQUIRE(transport.requests.size() == 2);
  REQUIRE(transport.requests[0].url == "https://api.anthropic.com/v1/messages");
  REQUIRE(header_value(transport.requests[0], "x-api-key") == "anthropic-secret");
  REQUIRE(transport.requests[1].url == "https://api.openai.com/v1/responses");
  REQUIRE(header_value(transport.requests[1], "authorization") == "Bearer openai-secret");
  REQUIRE(json::parse(transport.requests[1].body_json).at("model") == "gpt-main");
}

TEST_CASE("protocol transport maps HTTP status errors without response bodies", "[unit][provider][protocol]") {
  SECTION("auth") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 401, .headers = {}, .body_json = "{}"}}};
    auto profiles = route_profiles(provider::ProtocolKind::openai_responses);
    auto route = profiles.route();
    auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route.primary, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::auth);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"401"});
    });
  }

  SECTION("rate limited") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 429, .headers = {}, .body_json = "{}"}}};
    auto profiles = route_profiles(provider::ProtocolKind::openai_responses);
    auto route = profiles.route();
    auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route.primary, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::rate_limit);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"429"});
    });
  }

  SECTION("upstream") {
    RecordingTransport transport{
        {provider::ProtocolHttpResponse{.status_code = 503, .headers = {}, .body_json = "{}"}}};
    auto profiles = route_profiles(provider::ProtocolKind::openai_responses);
    auto route = profiles.route();
    auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
    REQUIRE(system.has_value());

    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await (*system)->send(request(), route.primary, nullptr);

      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::upstream);
      REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"503"});
    });
  }
}

TEST_CASE("protocol system rejects target identity changes before transport", "[unit][provider][protocol]") {
  RecordingTransport transport{{anthropic_response()}};
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  std::string_view field;
  std::string_view expected;
  SECTION("unknown profile") {
    route.primary.profile = "missing";
    field = "profile";
    expected = "missing";
  }
  SECTION("model mismatch") {
    route.primary.model = "other-model";
    field = "route_model";
    expected = "other-model";
  }
  SECTION("protocol mismatch") {
    route.primary.protocol = provider::ProtocolKind::openai_responses;
    field = "route_protocol";
    expected = "openai_responses";
  }
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, nullptr);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::config);
    REQUIRE(context_value(response.error(), field) == expected);
  });

  REQUIRE(transport.requests.empty());
}

TEST_CASE("protocol system retries primary before selecting fallback credentials and policy",
          "[unit][provider][protocol]") {
  const auto unavailable = provider::ProtocolHttpResponse{.status_code = 503, .headers = {}, .body_json = "{}"};
  RecordingTransport transport{{unavailable, unavailable, openai_response()}};
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  profiles.fallbacks.push_back(route_profiles(provider::ProtocolKind::openai_responses).primary);
  const auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto req = request();
    req.retry.max_attempts = 2;
    req.retry.initial_backoff = std::chrono::milliseconds{0};
    req.thinking_budget = 1024;
    req.max_tokens = 4096;
    auto outcome = co_await provider::execution::run(**system, std::move(req), route);
    auto& response = outcome.response;

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "openai ok");
    REQUIRE(response->model_used == "gpt-main");
    REQUIRE(outcome.target.profile == "openai-main");
    REQUIRE(outcome.target.fallback);
  });

  REQUIRE(transport.requests.size() == 3);
  REQUIRE(transport.requests[0].url == "https://api.anthropic.com/v1/messages");
  REQUIRE(transport.requests[1].url == transport.requests[0].url);
  REQUIRE(header_value(transport.requests[1], "x-api-key") == "anthropic-secret");
  REQUIRE(json::parse(transport.requests[1].body_json).at("thinking").at("budget_tokens") == 1024);
  REQUIRE(transport.requests[2].url == "https://api.openai.com/v1/responses");
  REQUIRE(header_value(transport.requests[2], "authorization") == "Bearer openai-secret");
  const auto fallback_body = json::parse(transport.requests[2].body_json);
  REQUIRE(fallback_body.at("model") == "gpt-main");
  REQUIRE_FALSE(fallback_body.contains("thinking"));
}

TEST_CASE("protocol transport system streams Anthropic deltas through the decoder", "[unit][provider][protocol][sse]") {
  StreamingTransport transport;
  transport.events = text_turn_events();
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "anthropic ok");
    REQUIRE(response->stop_reason == core::StopReason::end_turn);
    REQUIRE(response->usage.input_tokens == 4);
    REQUIRE(response->usage.output_tokens == 2);
    REQUIRE(response->model_used == std::optional<std::string>{"claude-sonnet"});
  });

  REQUIRE(transport.streaming_requests.size() == 1);
  REQUIRE(transport.body_requests.empty());
  REQUIRE(json::parse(transport.streaming_requests.front().body_json).at("stream") == true);
  REQUIRE(sink.log == std::vector<std::string>{"text:anthropic", "text: ok", "done"});
}

TEST_CASE("protocol transport system assembles a streamed tool_use turn", "[unit][provider][protocol][sse]") {
  StreamingTransport transport;
  transport.events = tool_turn_events();
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE(response.has_value());
    REQUIRE(response->stop_reason == core::StopReason::tool_use);
    const auto& tool = std::get<core::ToolUseContent>(response->blocks.front());
    REQUIRE(tool.id == "toolu_1");
    REQUIRE(tool.name == "get_weather");
    REQUIRE(json::parse(tool.input_json).at("city") == "NYC");
  });

  REQUIRE(transport.streaming_requests.size() == 1);
  REQUIRE(sink.log == std::vector<std::string>{
                          "tool_start:toolu_1:get_weather",
                          R"(tool_delta:toolu_1:{"city":"NYC"})",
                          "done",
                      });
}

TEST_CASE("protocol transport system keeps the body path when the transport cannot stream",
          "[unit][provider][protocol][sse]") {
  RecordingTransport transport{{anthropic_response()}};  // supports_streaming() defaults to false.
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "anthropic ok");
  });

  REQUIRE(transport.requests.size() == 1);
  REQUIRE(json::parse(transport.requests.front().body_json).at("stream") == false);
  // The body path still drives only the terminal on_done, never streaming deltas.
  REQUIRE(sink.log == std::vector<std::string>{"done"});
}

TEST_CASE("protocol transport system streams OpenAI Responses deltas when streaming is available",
          "[unit][provider][protocol][sse]") {
  StreamingTransport transport;
  transport.events = openai_text_turn_events();
  auto profiles = route_profiles(provider::ProtocolKind::openai_responses);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE(response.has_value());
    REQUIRE(std::get<core::TextContent>(response->blocks.front()).text == "openai ok");
    REQUIRE(response->usage.output_tokens == 3);
  });

  REQUIRE(transport.streaming_requests.size() == 1);
  REQUIRE(transport.body_requests.empty());
  REQUIRE(json::parse(transport.streaming_requests.front().body_json).at("stream") == true);
  REQUIRE(sink.log == std::vector<std::string>{"text:openai", "text: ok", "done"});
}

TEST_CASE("protocol transport system maps a streaming HTTP error", "[unit][provider][protocol][sse]") {
  StreamingTransport transport;
  transport.stream_status = 503;
  transport.stream_body = "{}";
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::upstream);
    REQUIRE(context_value(response.error(), "http_status") == std::optional<std::string_view>{"503"});
  });

  REQUIRE(transport.streaming_requests.size() == 1);
}

TEST_CASE("protocol transport system surfaces a streamed error event", "[unit][provider][protocol][sse]") {
  StreamingTransport transport;
  transport.events = {
      {"message_start",
       R"({"type":"message_start","message":{"id":"msg_3","type":"message","role":"assistant","model":"claude-sonnet","content":[],"stop_reason":null,"usage":{"input_tokens":4,"output_tokens":1}}})"},
      {"error", R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"},
  };
  auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
  auto route = profiles.route();
  auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
  REQUIRE(system.has_value());

  CapturingSink sink;
  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await (*system)->send(request(), route.primary, &sink);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::upstream);
  });

  REQUIRE(sink.done == std::nullopt);
}

TEST_CASE("protocol system joins cancelled transport while another profile completes",
          "[unit][provider][protocol][sse][lifetime]") {
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    PausingTransport transport{io.get_executor()};
    auto profiles = route_profiles(provider::ProtocolKind::anthropic_messages);
    profiles.fallbacks.push_back(route_profiles(provider::ProtocolKind::openai_responses).primary);
    const auto route = profiles.route();
    auto system = provider::make_protocol_system(transport, std::move(profiles), test_secret);
    REQUIRE(system.has_value());

    CapturingSink primary_sink;
    CapturingSink fallback_sink;
    asio::cancellation_signal cancellation;
    async::Channel<int> completed{io.get_executor(), 2};
    std::optional<core::Result<provider::Response>> primary;
    std::optional<core::Result<provider::Response>> fallback;
    std::exception_ptr primary_failure;
    std::exception_ptr fallback_failure;
    asio::co_spawn(
        io,
        (*system)->send(request(), route.primary, &primary_sink),
        asio::bind_cancellation_slot(cancellation.slot(),
                                     [&](std::exception_ptr failure, core::Result<provider::Response> result) {
                                       primary_failure = failure;
                                       primary = std::move(result);
                                       REQUIRE(completed.try_send(0).has_value());
                                     }));
    asio::co_spawn(io,
                   (*system)->send(request(), route.fallbacks.front(), &fallback_sink),
                   [&](std::exception_ptr failure, core::Result<provider::Response> result) {
                     fallback_failure = failure;
                     fallback = std::move(result);
                     REQUIRE(completed.try_send(1).has_value());
                   });

    auto first = co_await transport.started.receive();
    auto second = co_await transport.started.receive();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    for (const auto* sent : {&*first, &*second}) {
      if (sent->url.ends_with("/messages")) {
        REQUIRE(header_value(*sent, "x-api-key") == "anthropic-secret");
      } else {
        REQUIRE(sent->url == "https://api.openai.com/v1/responses");
        REQUIRE(header_value(*sent, "authorization") == "Bearer openai-secret");
      }
    }

    cancellation.emit(asio::cancellation_type::terminal);
    auto cleaning = co_await transport.cleanup_started.receive();
    REQUIRE(cleaning.has_value());
    REQUIRE_FALSE(primary.has_value());
    REQUIRE_FALSE(fallback.has_value());
    REQUIRE(transport.fallback_release.try_send(0).has_value());
    auto fallback_done = co_await completed.receive();
    REQUIRE(fallback_done == 1);
    REQUIRE(fallback_failure == nullptr);
    REQUIRE(fallback.has_value());
    REQUIRE(fallback->has_value());
    REQUIRE(std::get<core::TextContent>((**fallback).blocks.front()).text == "openai ok");
    REQUIRE(fallback_sink.log == std::vector<std::string>{"text:openai", "text: ok", "done"});
    REQUIRE_FALSE(primary.has_value());
    REQUIRE_FALSE(transport.cleanup_finished);

    REQUIRE(transport.cleanup_release.try_send(0).has_value());
    auto primary_done = co_await completed.receive();
    REQUIRE(primary_done == 0);
    REQUIRE(primary_failure == nullptr);
    REQUIRE(primary.has_value());
    REQUIRE_FALSE(primary->has_value());
    REQUIRE(primary->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(context_value(primary->error(), "provider_profile") == "anthropic-main");
    REQUIRE(primary_sink.log == std::vector<std::string>{"text:anthropic"});
    REQUIRE(transport.cleanup_finished);
    system->reset();
  });
}
