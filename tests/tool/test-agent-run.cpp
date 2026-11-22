#include <array>
#include <string>
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

TEST_CASE("AgentRun rejects inputs outside the dynamic task contract", "[unit][tool][collaboration]") {
  std::string input;
  SECTION("preset selector is unsupported") {
    input = R"({"agent":"stranger","prompt":"inspect"})";
  }
  SECTION("authority override") {
    input = R"({"prompt":"inspect","mode":"permissive"})";
  }
  SECTION("scope override") {
    input = R"({"prompt":"inspect","scope_key":"elsewhere"})";
  }
  SECTION("session reuse") {
    input = R"({"prompt":"inspect","session_id":"parent"})";
  }
  SECTION("missing prompt") {
    input = "{}";
  }
  SECTION("unbound background option") {
    input = R"({"prompt":"inspect","background":true})";
  }
  SECTION("empty prompt") {
    input = R"({"prompt":""})";
  }
  SECTION("oversized prompt") {
    input = nlohmann::json{{"prompt", std::string(16385, 'x')}}.dump();
  }
  orangutan::tests::run_async([&input](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_agent_run(registry));
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    bool executed = false;
    context.agent_run = [&executed](tool::AgentRunRequest,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      executed = true;
      co_return tool::Output::text_only("child result");
    };

    auto result = co_await registry.dispatch(tool::AGENT_RUN_NAME, input, context);

    REQUIRE_FALSE(executed);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("AgentRun requires only a task and uses runtime admission", "[unit][tool][collaboration]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_agent_run(registry));
    const auto* definition = registry.find(tool::AGENT_RUN_NAME);
    REQUIRE(definition != nullptr);
    const auto schema = nlohmann::json::parse(definition->input_schema_json);
    REQUIRE(schema["required"] == nlohmann::json::array({"prompt"}));
    CHECK_FALSE(schema["properties"].contains("agent"));
    CHECK_FALSE(schema["properties"].contains("background"));
    REQUIRE(definition->required_capabilities.empty());
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit, "scope", "parent", "owner");
    context.mode = permission::Mode::permissive;
    tool::AgentRunRequest received;
    context.agent_run = [&received](tool::AgentRunRequest request,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      received = std::move(request);
      co_return tool::Output::text_only("review complete");
    };
    auto snapshot = tool::DispatchContext::for_now(context, false);

    auto result = co_await registry.dispatch(tool::AGENT_RUN_NAME, R"({"prompt":"check changes"})", snapshot);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "review complete");
    REQUIRE(received.prompt == "check changes");
    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].tool_name == "AgentRun");
    REQUIRE(audit.events()[0].identity == "owner");
    REQUIRE(audit.events()[0].outcome == permission::AuditOutcome::allow);
  });
}

TEST_CASE("Background AgentRun validates options before calling the host", "[unit][tool][background]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_agent_run(registry, true));
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    int calls = 0;
    context.agent_run = [&calls](tool::AgentRunRequest request,
                                 tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++calls;
      CHECK(request.background);
      CHECK(request.label == "检查");
      co_return tool::Output::text_only("accepted");
    };
    for (const auto& extra : {nlohmann::json{{"background", "true"}},
                              nlohmann::json{{"label", ""}},
                              nlohmann::json{{"label", "bad\nlabel"}},
                              nlohmann::json{{"label", std::string(121, 'x')}}}) {
      auto input = nlohmann::json{{"prompt", "inspect"}};
      input.update(extra);
      auto result = co_await registry.dispatch(tool::AGENT_RUN_NAME, input.dump(), context);
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
    }
    CHECK(calls == 0);
    auto accepted = co_await registry.dispatch(tool::AGENT_RUN_NAME,
                                               R"({"prompt":"inspect","background":true,"label":"检查"})",
                                               context);
    REQUIRE(accepted.has_value());
    CHECK(calls == 1);
  });
}

TEST_CASE("Task tools validate windows and preserve handlers in dispatch snapshots", "[unit][tool][background]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_task_tools(registry));
    permission::NullAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit);
    context.mode = permission::Mode::permissive;
    int reads = 0, cancels = 0;
    context.task_get = [&reads](tool::TaskRequest request,
                                tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++reads;
      CHECK(request.offset == 3);
      CHECK(request.max_bytes == 12);
      co_return tool::Output::text_only("result");
    };
    context.task_cancel = [&cancels](tool::TaskRequest,
                                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++cancels;
      co_return tool::Output::text_only("cancelling");
    };
    auto snapshot = tool::DispatchContext::for_now(context, false);
    const std::string id = "00000000000000000000000000000001";
    for (const auto& extra : {nlohmann::json{{"task_id", "bad"}},
                              nlohmann::json{{"offset", -1}},
                              nlohmann::json{{"offset", 1.5}},
                              nlohmann::json{{"max_bytes", 0}},
                              nlohmann::json{{"max_bytes", 16385}},
                              nlohmann::json{{"scope", "other"}}}) {
      auto input = nlohmann::json{{"task_id", id}};
      input.update(extra);
      auto result = co_await registry.dispatch(tool::TASK_GET_NAME, input.dump(), snapshot);
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
    }
    CHECK(reads == 0);
    auto read = co_await registry.dispatch(tool::TASK_GET_NAME,
                                           nlohmann::json{{"task_id", id}, {"offset", 3}, {"max_bytes", 12}}.dump(),
                                           snapshot);
    REQUIRE(read.has_value());
    auto cancelled =
        co_await registry.dispatch(tool::TASK_CANCEL_NAME, nlohmann::json{{"task_id", id}}.dump(), snapshot);
    REQUIRE(cancelled.has_value());
    CHECK(reads == 1);
    CHECK(cancels == 1);
  });
}
