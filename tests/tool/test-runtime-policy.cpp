#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <oran/async.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

using namespace orangutan;

TEST_CASE("Internal tools ignore generic rules and never ask for approval", "[tool][runtime-policy]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_memory_tools(registry));
    REQUIRE(tool::register_agent_run(registry, true));
    REQUIRE(tool::register_task_tools(registry));
    permission::RuleSet rules;
    rules.push_back({.verdict = permission::Verdict::deny, .tool_pattern = "*"});
    rules.push_back({.verdict = permission::Verdict::ask, .tool_pattern = "*"});
    permission::RecordingAuditSink audit;
    hook::Bus hooks;
    int approvals = 0, effects = 0;
    hooks.subscribe({.id = "approval-observer",
                     .observe = [&](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
                       ++approvals;
                       co_return;
                     }},
                    {hook::Event::permission_ask_rendered});
    auto context = tool::DispatchContext::for_now(io.get_executor(), rules, audit);
    context.bus = &hooks;
    context.parent_policy = permission::PolicyView{rules, permission::Mode::strict};
    const auto handle = [&effects](auto, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++effects;
      co_return tool::Output::text_only("runtime result");
    };
    context.memory_recall = handle;
    context.memory_remember = handle;
    context.memory_forget = handle;
    context.agent_run = handle;
    context.task_get = handle;
    context.task_cancel = handle;
    const std::array calls{
        std::pair{tool::kMemoryRecallName, "{}"},
        std::pair{tool::kMemoryRememberName, R"({"id":"n","kind":"user","title":"Preference","body":"Be concise"})"},
        std::pair{tool::kMemoryForgetName, R"({"id":"n"})"},
        std::pair{tool::AGENT_RUN_NAME, R"({"prompt":"inspect","background":true})"},
        std::pair{tool::TASK_GET_NAME, R"({"task_id":"00000000000000000000000000000001"})"},
        std::pair{tool::TASK_CANCEL_NAME, R"({"task_id":"00000000000000000000000000000001"})"}};
    for (auto mode : {permission::Mode::strict,
                      permission::Mode::default_,
                      permission::Mode::sandboxed,
                      permission::Mode::permissive}) {
      context.mode = mode;
      for (const auto& [name, input] : calls) {
        auto result = co_await registry.dispatch(name, input, context);
        REQUIRE(result.has_value());
        CHECK(result->text == "runtime result");
        CHECK(audit.events().back().reason == "runtime_tool");
      }
    }
    CHECK(effects == 24);
    CHECK(approvals == 0);

    // Runtime registration is explicit: a custom tool with no capabilities
    // still uses the ordinary permission boundary.
    REQUIRE(registry.add(core::ToolDef::with_no_input("Custom", "custom effect"), handle));
    auto denied = co_await registry.dispatch("Custom", "{}", context);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().kind() == core::ErrorKind::permission_denied);
    CHECK(effects == 24);

    const std::vector<std::string> disabled;
    context.active_tools = disabled;
    auto unavailable = co_await registry.dispatch(tool::kMemoryRecallName, "{}", context);
    REQUIRE_FALSE(unavailable.has_value());
    CHECK(unavailable.error().kind() == core::ErrorKind::not_found);
    CHECK(effects == 24);
  });
}

TEST_CASE("Runtime registration cannot conceal external capability requirements", "[tool][runtime-policy]") {
  tool::Registry registry;
  auto definition = core::ToolDef::with_no_input("Internal", "invalid internal tool");
  definition.required_capabilities = {core::Capability::write_file};
  auto result = registry.add_prepared(
      std::move(definition),
      [](std::string_view) -> core::Result<tool::PreparedCall> {
        return tool::PreparedCall{
            .path = std::nullopt,
            .execute = [](tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
              co_return tool::Output::text_only("unreachable");
            }};
      },
      tool::DispatchPolicy::runtime);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
}
