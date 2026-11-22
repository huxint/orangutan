// tests/provider/test_fake.cpp — FakeProvider scripted-turn coverage.
//
// Spec 0017 acceptance criteria need a fake provider that pins the loop
// against deterministic Response/Error/sink shapes. These cases lock in the
// fake's surface ahead of the first loop slice so any later behaviour drift
// surfaces here instead of inside the loop tests.

#include <oran/provider.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace prov = orangutan::provider;
namespace test = orangutan::tests;

struct CapturingSink final : prov::EventSink {
  std::vector<std::string> events;

  void on_text_delta(std::string_view delta) override {
    events.emplace_back(std::string{"text:"} + std::string{delta});
  }
  void on_thinking_delta(std::string_view delta) override {
    events.emplace_back(std::string{"thinking:"} + std::string{delta});
  }
  void on_tool_start(std::string_view id, std::string_view name) override {
    events.emplace_back(std::string{"tool_start:"} + std::string{id} + ":" + std::string{name});
  }
  void on_tool_delta(std::string_view id, std::string_view input_delta) override {
    events.emplace_back(std::string{"tool_delta:"} + std::string{id} + ":" + std::string{input_delta});
  }
  void on_done(core::StopReason reason) override {
    events.emplace_back(std::string{"done:"} + std::string{core::enum_name(reason)});
  }
};

prov::ModelTarget default_target() {
  return prov::ModelTarget{
      .profile = "fake",
      .model = "fake-1",
      .protocol = prov::ProtocolKind::anthropic_messages,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

prov::Request empty_request() {
  return prov::Request{};
}

}  // namespace

TEST_CASE("FakeProvider replays a complete scripted Response", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    prov::Response canned{
        .blocks = {core::TextContent{.text = "hello"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = prov::Usage{.input_tokens = 10,
                             .output_tokens = 2,
                             .cache_creation_tokens = 0,
                             .cache_read_tokens = 0,
                             .cost_estimate = std::nullopt},
        .model_used = std::nullopt,
    };
    plan.push_back(
        prov::ScriptedTurn{.response = std::move(canned), .deltas = {}, .error = std::nullopt, .latency = {}});

    prov::FakeProvider fake{std::move(plan)};
    CapturingSink sink;

    auto result = co_await fake.send(empty_request(), default_target(), &sink);
    REQUIRE(result.has_value());
    REQUIRE(result->blocks.size() == 1);
    REQUIRE(std::holds_alternative<core::TextContent>(result->blocks[0]));
    REQUIRE(std::get<core::TextContent>(result->blocks[0]).text == "hello");
    REQUIRE(result->stop_reason == core::StopReason::end_turn);
    REQUIRE(result->usage.input_tokens == 10);
    REQUIRE(result->usage.output_tokens == 2);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events.front() == "done:end_turn");

    REQUIRE(fake.turns_consumed() == 1);
    REQUIRE(fake.exhausted());
  });
}

TEST_CASE("FakeProvider assembles a Response from streamed deltas", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::StreamDelta> deltas;
    deltas.push_back(prov::TextDelta{.text = "hel"});
    deltas.push_back(prov::TextDelta{.text = "lo "});
    deltas.push_back(prov::ToolStart{.id = "t1", .name = "FileRead"});
    deltas.push_back(prov::ToolInputDelta{.id = "t1", .input_delta = R"({"pa)"});
    deltas.push_back(prov::ToolInputDelta{.id = "t1", .input_delta = R"(th":"x"})"});
    deltas.push_back(prov::StreamEnd{
        .stop_reason = core::StopReason::tool_use,
        .usage = prov::Usage{.input_tokens = 1,
                             .output_tokens = 5,
                             .cache_creation_tokens = 0,
                             .cache_read_tokens = 0,
                             .cost_estimate = std::nullopt},
        .model_used = std::string{"fake-1"},
    });

    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{.response = std::nullopt,
                                      .deltas = std::move(deltas),
                                      .error = std::nullopt,
                                      .latency = {}});

    prov::FakeProvider fake{std::move(plan)};
    CapturingSink sink;

    auto result = co_await fake.send(empty_request(), default_target(), &sink);
    REQUIRE(result.has_value());
    REQUIRE(result->stop_reason == core::StopReason::tool_use);
    REQUIRE(result->usage.input_tokens == 1);
    REQUIRE(result->usage.output_tokens == 5);
    REQUIRE(result->model_used == std::string{"fake-1"});

    REQUIRE(result->blocks.size() == 2);
    REQUIRE(std::holds_alternative<core::TextContent>(result->blocks[0]));
    REQUIRE(std::get<core::TextContent>(result->blocks[0]).text == "hello ");
    REQUIRE(std::holds_alternative<core::ToolUseContent>(result->blocks[1]));
    const auto& tool = std::get<core::ToolUseContent>(result->blocks[1]);
    REQUIRE(tool.id == "t1");
    REQUIRE(tool.name == "FileRead");
    REQUIRE(tool.input_json == R"({"path":"x"})");

    REQUIRE(sink.events.size() == 6);
    REQUIRE(sink.events[0] == "text:hel");
    REQUIRE(sink.events[1] == "text:lo ");
    REQUIRE(sink.events[2] == "tool_start:t1:FileRead");
    REQUIRE(sink.events[3] == std::string{"tool_delta:t1:"} + R"({"pa)");
    REQUIRE(sink.events[4] == std::string{"tool_delta:t1:"} + R"(th":"x"})");
    REQUIRE(sink.events[5] == "done:tool_use");
  });
}

TEST_CASE("FakeProvider injects a scripted error", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{.response = std::nullopt,
                                      .deltas = {},
                                      .error = core::Error::network("upstream timeout"),
                                      .latency = {}});

    prov::FakeProvider fake{std::move(plan)};
    auto result = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(result.error().retryable());
  });
}

TEST_CASE("FakeProvider rejects a scripted turn with no body", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{});

    prov::FakeProvider fake{std::move(plan)};
    auto result = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
  });
}

TEST_CASE("FakeProvider exhausts the plan and reports an internal error", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{.response = prov::Response{.blocks = {},
                                                                 .stop_reason = core::StopReason::end_turn,
                                                                 .usage = {},
                                                                 .model_used = std::nullopt},
                                      .deltas = {},
                                      .error = std::nullopt,
                                      .latency = {}});

    prov::FakeProvider fake{std::move(plan)};
    REQUIRE(fake.plan_size() == 1);

    auto first = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE(first.has_value());

    auto second = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().kind() == core::ErrorKind::internal);

    REQUIRE(fake.turns_consumed() == 2);
    REQUIRE(fake.exhausted());
  });
}

TEST_CASE("FakeProvider drives multiple turns in plan order", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{
        .response =
            prov::Response{
                .blocks = {core::TextContent{.text = "first"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = {},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    plan.push_back(prov::ScriptedTurn{
        .response =
            prov::Response{
                .blocks = {core::TextContent{.text = "second"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = {},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });

    prov::FakeProvider fake{std::move(plan)};

    auto first = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE(first.has_value());
    REQUIRE(std::get<core::TextContent>(first->blocks.front()).text == "first");
    REQUIRE(first->stop_reason == core::StopReason::tool_use);

    auto second = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE(second.has_value());
    REQUIRE(std::get<core::TextContent>(second->blocks.front()).text == "second");
    REQUIRE(second->stop_reason == core::StopReason::end_turn);

    REQUIRE(fake.turns_consumed() == 2);
    REQUIRE(fake.exhausted());
  });
}

TEST_CASE("FakeProvider respects parent cancellation during scripted latency", "[unit][provider][fake]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  std::vector<prov::ScriptedTurn> plan;
  plan.push_back(prov::ScriptedTurn{
      .response = prov::Response{.blocks = {},
                                 .stop_reason = core::StopReason::end_turn,
                                 .usage = {},
                                 .model_used = std::nullopt},
      .deltas = {},
      .error = std::nullopt,
      .latency = 1s,
  });

  prov::FakeProvider fake{std::move(plan)};

  std::optional<core::Result<prov::Response>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<prov::Response>> {
        co_return co_await fake.send(empty_request(), default_target(), nullptr);
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<prov::Response> r) {
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
  REQUIRE(fake.turns_consumed() == 1);
}

TEST_CASE("FakeProvider tolerates a null sink during streaming", "[unit][provider][fake]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<prov::StreamDelta> deltas;
    deltas.push_back(prov::TextDelta{.text = "ok"});
    deltas.push_back(
        prov::StreamEnd{.stop_reason = core::StopReason::end_turn, .usage = std::nullopt, .model_used = std::nullopt});

    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{.response = std::nullopt,
                                      .deltas = std::move(deltas),
                                      .error = std::nullopt,
                                      .latency = {}});

    prov::FakeProvider fake{std::move(plan)};
    auto result = co_await fake.send(empty_request(), default_target(), nullptr);
    REQUIRE(result.has_value());
    REQUIRE(result->blocks.size() == 1);
    REQUIRE(std::get<core::TextContent>(result->blocks.front()).text == "ok");
    REQUIRE(result->stop_reason == core::StopReason::end_turn);
  });
}
