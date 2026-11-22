#include <oran/agent/loop.hpp>

#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/provider/system.hpp>

#include "../../src/oran-agent/context.hpp"
#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;

namespace {
const std::string SUMMARY =
    "Objective:\nShip parser.\nConstraints:\nNo new dependencies.\nDecisions:\nUse existing parser "
    "[1].\nCompleted:\nReviewed code.\nPending:\nRun tests.\nReferences:\nsrc/parser.cpp [2]\n";
provider::Response response(std::string text) {
  return {.blocks = {core::TextContent{.text = std::move(text)}},
          .usage = {.input_tokens = 5, .output_tokens = 3, .cost_estimate = std::nullopt},
          .model_used = "test"};
}
provider::Route route() {
  return {.primary = {.profile = "test", .model = "test", .thinking_budget = std::nullopt, .cache = std::nullopt},
          .fallbacks = {}};
}
std::vector<core::Message> history() {
  std::vector<core::Message> messages;
  for (int i = 0; i < 12; ++i) {
    messages.push_back(core::Message::user_text("No new dependencies. " + std::string(350, 'u')));
    messages.push_back(core::Message::assistant_text(std::string(350, 'a')));
  }
  return messages;
}
class SummarizingProvider final : public provider::System {
public:
  mutable std::vector<provider::Request> requests;
  bool fail_summary{};
  bool cancel_summary{};
  bool cancelled_response{};
  async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget, provider::EventSink*) const override {
    const bool summary = request.system_prompt.value_or("").starts_with("Produce a bounded session handoff");
    requests.push_back(std::move(request));
    if (summary && cancel_summary)
      co_return std::unexpected(core::Error::cancelled());
    auto result = response(summary ? (fail_summary ? "invalid" : SUMMARY) : "done");
    if (summary && cancelled_response)
      result.stop_reason = core::StopReason::cancelled;
    co_return result;
  }
};
}  // namespace

TEST_CASE("Context compaction keeps original transcript and accounts for summary usage", "[agent][context]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SummarizingProvider backend;
    agent::Loop loop{backend, route()};
    auto messages = history();
    messages.push_back(core::Message::user_text("Continue"));
    agent::RunTurnInputs inputs{.system_preamble = "Agent", .conversation_tail = messages};
    inputs.context = {.max_tokens = 16384, .summary_max_bytes = 512};
    inputs.max_tokens = 512;
    auto result = co_await loop.run_turn(inputs);
    REQUIRE(result);
    REQUIRE(result->checkpoint.covered_sequence > 0);
    REQUIRE(result->checkpoint.summary == SUMMARY);
    REQUIRE(result->transcript.size() == messages.size() + 1);
    REQUIRE(result->transcript.front() == messages.front());
    REQUIRE(backend.requests.size() == 2);
    REQUIRE(backend.requests.front().tools.empty());
    REQUIRE_FALSE(backend.requests.front().stream);
    REQUIRE(backend.requests.back().messages.front().role == core::Role::user);
    REQUIRE(backend.requests.back().messages.size() < messages.size());
    REQUIRE(result->usage.input_tokens == 10);
    REQUIRE(result->usage.output_tokens == 6);
  });
}

TEST_CASE("Invalid soft summaries retain context and cancelled summaries stop the turn", "[agent][context]") {
  for (int cancellation : {0, 1, 2}) {
    test::run_async([cancellation](asio::io_context&) -> async::Awaitable<void> {
      SummarizingProvider backend;
      backend.fail_summary = true;
      backend.cancel_summary = cancellation == 1;
      backend.cancelled_response = cancellation == 2;
      agent::Loop loop{backend, route()};
      auto messages = history();
      messages.push_back(core::Message::user_text("Continue"));
      agent::RunTurnInputs inputs{.system_preamble = "Agent", .conversation_tail = messages};
      inputs.context = {.max_tokens = 16384, .summary_max_bytes = 512};
      inputs.max_tokens = 512;
      auto result = co_await loop.run_turn(inputs);
      if (cancellation != 0) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().kind() == core::ErrorKind::cancelled);
        REQUIRE(backend.requests.size() == 1);
      } else {
        REQUIRE(result);
        REQUIRE(result->checkpoint.covered_sequence == 0);
        REQUIRE(backend.requests.back().messages == messages);
      }
    });
  }
}

TEST_CASE("Context compaction never splits a parallel tool group", "[agent][context]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    agent::detail::ContextView view;
    view.messages = history();
    view.messages.push_back(
        core::Message{.role = core::Role::assistant,
                      .blocks = {core::ToolUseContent{.id = "a", .name = "Read", .input_json = "{}"},
                                 core::ToolUseContent{.id = "b", .name = "Read", .input_json = "{}"}},
                      .created_at = std::nullopt});
    view.messages.push_back(core::Message{
        .role = core::Role::tool,
        .blocks = {core::ToolResultContent{.tool_use_id = "a", .output = "first", .data_json = std::nullopt}},
        .created_at = std::nullopt});
    view.messages.push_back(core::Message{
        .role = core::Role::tool,
        .blocks = {core::ToolResultContent{.tool_use_id = "b", .output = "second", .data_json = std::nullopt}},
        .created_at = std::nullopt});
    provider::Request frame;
    frame.max_tokens = 512;
    agent::detail::SummarySender send = [](provider::Request) -> async::Awaitable<core::Result<provider::Response>> {
      co_return response(SUMMARY);
    };
    auto fitted =
        co_await agent::detail::fit_context(view, frame, {.max_tokens = 16384, .summary_max_bytes = 512}, send);
    REQUIRE(fitted);
    REQUIRE(view.checkpoint.covered_sequence > 0);
    REQUIRE(view.checkpoint.covered_sequence <= 24);
    REQUIRE(view.messages[view.messages.size() - 3].blocks.size() == 2);
    REQUIRE(std::get<core::ToolResultContent>(view.messages.back().blocks.front()).tool_use_id == "b");
  });
}

TEST_CASE("An indivisible oversized request fails before provider work", "[agent][context]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SummarizingProvider backend;
    agent::Loop loop{backend, route()};
    const auto messages = std::vector{core::Message::user_text(std::string(20000, 'x'))};
    agent::RunTurnInputs inputs{.system_preamble = "Agent", .conversation_tail = messages};
    inputs.context = {.max_tokens = 16384, .summary_max_bytes = 512};
    inputs.max_tokens = 512;
    auto result = co_await loop.run_turn(inputs);
    REQUIRE_FALSE(result);
    REQUIRE(backend.requests.empty());
  });
}

TEST_CASE("Image context accounting uses image allowance instead of base64 text length", "[agent][context]") {
  provider::Request request;
  request.messages.push_back(core::Message{.role = core::Role::user,
                                           .blocks = {core::ImageContent{"image/png", "aW1hZ2U="}},
                                           .created_at = std::nullopt});
  const auto small = agent::estimate_input_tokens(request);
  std::get<core::ImageContent>(request.messages.front().blocks.front()).data_base64.assign(1024 * 1024, 'A');
  CHECK(agent::estimate_input_tokens(request) == small);
  CHECK(small > 4096);
  CHECK(small < 131072);
}

TEST_CASE("Image compaction preserves original bytes while keeping handoffs textual", "[agent][context]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SummarizingProvider backend;
    agent::Loop loop{backend, route()};
    auto messages = history();
    const core::ImageContent image{"image/png", std::string(600 * 1024, 'A')};
    messages.front().blocks.emplace_back(image);
    messages.push_back(core::Message::user_text("Continue"));
    agent::RunTurnInputs inputs{.system_preamble = "Agent", .conversation_tail = messages};
    inputs.context = {.max_tokens = 16384, .summary_max_bytes = 512};
    inputs.max_tokens = 512;
    auto result = co_await loop.run_turn(inputs);
    REQUIRE(result);
    CHECK(result->checkpoint.covered_sequence > 0);
    CHECK(result->transcript.front() == messages.front());
    REQUIRE(backend.requests.size() >= 2);
    bool image_marker = false;
    for (const auto& message : backend.requests.front().messages) {
      for (const auto& block : message.blocks) {
        const auto* text = std::get_if<core::TextContent>(&block);
        REQUIRE(text);
        CHECK_FALSE(text->text.contains(image.data_base64));
        image_marker = image_marker || text->text.contains("[Image: image/png;");
      }
    }
    CHECK(image_marker);
  });
}
