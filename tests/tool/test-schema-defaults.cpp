#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;
using json = nlohmann::json;

namespace {

json with_declared_defaults(const json& schema, json input) {
  for (const auto& [name, property] : schema.at("properties").items()) {
    if (!input.contains(name) && property.contains("default")) {
      input[name] = property.at("default");
    }
  }
  return input;
}

}  // namespace

TEST_CASE("MemoryRecall declared defaults preserve all three selector modes", "[unit][tool][schema]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_recall(registry).has_value());
    const auto schema = json::parse(registry.find(tool::kMemoryRecallName)->input_schema_json);
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    std::vector<tool::MemoryRecallRequest> seen;
    context.memory_recall = [&seen](tool::MemoryRecallRequest request,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      seen.push_back(std::move(request));
      co_return tool::Output::text_only("read");
    };
    const auto inputs = std::array{json::object(), json{{"query", "workflow"}}, json{{"id", "reply-style"}}};
    const auto limits = std::array<std::size_t, 3>{20, 5, 1};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      seen.clear();
      auto omitted = co_await registry.dispatch(tool::kMemoryRecallName, inputs[i].dump(), context);
      REQUIRE(omitted.has_value());
      auto explicit_defaults = co_await registry.dispatch(tool::kMemoryRecallName,
                                                          with_declared_defaults(schema, inputs[i]).dump(),
                                                          context);
      REQUIRE(explicit_defaults.has_value());
      REQUIRE(seen.size() == 2);
      REQUIRE(seen[0] == seen[1]);
      REQUIRE(seen[0].limit == limits[i]);
      REQUIRE(seen[0].kinds == schema.at("properties").at("kinds").at("default").get<std::vector<std::string>>());
    }
  });
}

TEST_CASE("MemoryRemember advertised defaults match omitted metadata", "[unit][tool][schema]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_remember(registry).has_value());
    const auto schema = json::parse(registry.find(tool::kMemoryRememberName)->input_schema_json);
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    std::vector<tool::MemoryRememberRequest> seen;
    context.memory_remember = [&seen](tool::MemoryRememberRequest request,
                                      tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      seen.push_back(std::move(request));
      co_return tool::Output::text_only("saved");
    };
    const auto input =
        json{{"id", "reply-style"}, {"kind", "user"}, {"title", "Reply style"}, {"body", "Lead with the result."}};
    auto omitted = co_await registry.dispatch(tool::kMemoryRememberName, input.dump(), context);
    REQUIRE(omitted.has_value());
    auto explicit_defaults =
        co_await registry.dispatch(tool::kMemoryRememberName, with_declared_defaults(schema, input).dump(), context);
    REQUIRE(explicit_defaults.has_value());
    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0] == seen[1]);
    const auto& properties = schema.at("properties");
    REQUIRE(seen[0].importance == properties.at("importance").at("default").get<double>());
    REQUIRE(seen[0].tags == properties.at("tags").at("default").get<std::vector<std::string>>());
    REQUIRE(seen[0].linked_record_ids ==
            properties.at("linked_record_ids").at("default").get<std::vector<std::string>>());
  });
}

TEST_CASE("MemoryRecall and AgentRun enforce advertised UTF-8 byte caps before effects", "[unit][tool][schema]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_recall(registry).has_value());
    REQUIRE(tool::register_agent_run(registry).has_value());
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    std::size_t calls = 0;
    context.memory_recall = [&calls](tool::MemoryRecallRequest,
                                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++calls;
      co_return tool::Output::text_only("read");
    };
    context.agent_run = [&calls](tool::AgentRunRequest,
                                 tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++calls;
      co_return tool::Output::text_only("done");
    };
    for (const auto name : {tool::kMemoryRecallName, tool::AGENT_RUN_NAME}) {
      const auto field = name == tool::kMemoryRecallName ? "query" : "prompt";
      const auto schema = json::parse(registry.find(name)->input_schema_json);
      const auto cap = schema.at("properties").at(field).at("maxLength").get<std::size_t>();
      REQUIRE(cap >= 3);
      auto input = json{{field, std::string(cap - 3, 'x') + "中"}};
      calls = 0;
      auto exact = co_await registry.dispatch(name, input.dump(), context);
      REQUIRE(exact.has_value());
      REQUIRE(calls == 1);
      input[field].get_ref<std::string&>() += "x";
      auto oversized = co_await registry.dispatch(name, input.dump(), context);
      REQUIRE_FALSE(oversized.has_value());
      REQUIRE(oversized.error().kind() == core::ErrorKind::invalid_argument);
      REQUIRE(calls == 1);
    }
  });
}
