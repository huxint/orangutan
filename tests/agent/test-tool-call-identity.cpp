#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;

namespace {

provider::Route route() {
  return {.primary = {.profile = "fake", .model = "fake-1", .protocol = provider::ProtocolKind::anthropic_messages,
                       .thinking_budget = std::nullopt, .cache = std::nullopt},
          .fallbacks = {}};
}

provider::ScriptedTurn call_batch(std::vector<std::string> ids) {
  provider::Response response;
  response.stop_reason = core::StopReason::tool_use;
  for (auto& id : ids) {
    response.blocks.emplace_back(core::ToolUseContent{.id = std::move(id), .name = "Count", .input_json = "{}"});
  }
  return {.response = std::move(response), .deltas = {}, .error = std::nullopt};
}

provider::ScriptedTurn finished() {
  return {.response = provider::Response{.blocks = {core::TextContent{.text = "done"}},
                                        .stop_reason = core::StopReason::end_turn,
                                        .usage = {},
                                        .model_used = std::nullopt},
          .deltas = {},
          .error = std::nullopt};
}

void register_counter(tool::Registry& registry, std::size_t& calls) {
  auto registered = registry.add(
      core::ToolDef::with_no_input("Count", "Record an effect"),
      [&calls](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
        ++calls;
        co_return tool::Output::text_only("effect completed");
      });
  REQUIRE(registered.has_value());
}

}  // namespace

TEST_CASE("Loop rejects an entire batch with empty or duplicate tool IDs before effects", "[agent][tool-identity]") {
  const bool empty = GENERATE(false, true);
  const auto stop = GENERATE(core::StopReason::tool_use, core::StopReason::end_turn);
  test::run_async([empty, stop](asio::io_context& io) -> async::Awaitable<void> {
    auto malformed = call_batch({"first", empty ? "" : "first"});
    malformed.response->stop_reason = stop;
    provider::FakeProvider provider{{std::move(malformed), finished()}};
    agent::Loop loop{provider, route()};
    tool::Registry registry;
    std::size_t calls = 0;
    register_counter(registry, calls);
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    const auto catalog = registry.catalog();
    const auto tail = std::vector{core::Message::user_text("perform the operation")};
    auto result = co_await loop.run_turn({.tool_catalog = catalog,
                                          .conversation_tail = tail,
                                          .tools = &registry,
                                          .dispatch_context = &context});
    REQUIRE(calls == 0);
    REQUIRE(audit.events().empty());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    const auto reason = empty ? "empty_tool_use_id" : "duplicate_tool_use_id";
    REQUIRE(std::ranges::contains(result.error().context(), core::Error::ContextEntry{"reason", reason}));
    REQUIRE(provider.turns_consumed() == 1);
  });
}

TEST_CASE("Loop rejects replayed IDs across iterations before admitting any new batch effects",
          "[agent][tool-identity]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    provider::FakeProvider provider{{call_batch({"first"}), call_batch({"new", "first"}), finished()}};
    agent::Loop loop{provider, route()};
    tool::Registry registry;
    std::size_t calls = 0;
    register_counter(registry, calls);
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    const auto catalog = registry.catalog();
    const auto tail = std::vector{core::Message::user_text("perform the operation")};
    auto result = co_await loop.run_turn({.tool_catalog = catalog,
                                          .conversation_tail = tail,
                                          .tools = &registry,
                                          .dispatch_context = &context});
    REQUIRE(calls == 1);
    REQUIRE(audit.events().size() == 1);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    REQUIRE(std::ranges::contains(result.error().context(),
                                  core::Error::ContextEntry{"reason", "duplicate_tool_use_id"}));
    REQUIRE(provider.turns_consumed() == 2);
  });
}

TEST_CASE("Loop allows identical arguments with fresh IDs and keeps ID state local to one turn",
          "[agent][tool-identity]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    provider::FakeProvider provider{{call_batch({"first"}), call_batch({"second"}), finished(),
                                     call_batch({"first"}), finished()}};
    agent::Loop loop{provider, route()};
    tool::Registry registry;
    std::size_t calls = 0;
    register_counter(registry, calls);
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    const auto catalog = registry.catalog();
    const auto tail = std::vector{core::Message::user_text("perform the operation")};
    const auto inputs = agent::RunTurnInputs{.tool_catalog = catalog,
                                            .conversation_tail = tail,
                                            .tools = &registry,
                                            .dispatch_context = &context};
    auto first = co_await loop.run_turn(inputs);
    REQUIRE(first.has_value());
    REQUIRE(first->text == "done");
    REQUIRE(calls == 2);
    REQUIRE(first->transcript.size() == 6);
    auto second = co_await loop.run_turn(inputs);
    REQUIRE(second.has_value());
    REQUIRE(second->text == "done");
    REQUIRE(calls == 3);
  });
}
