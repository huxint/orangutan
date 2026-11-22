#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

using namespace orangutan;

TEST_CASE("Team tools validate authority fields, task revisions and bounded dynamic roles", "[tool][teams]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_team_tools(registry));
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), {}, audit, "scope", "leader", "owner");
    std::size_t called = 0;
    context.team = [&called](tool::TeamCommand,
                             tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      ++called;
      co_return tool::Output::text_only("accepted");
    };
    const std::array invalid{
        std::pair{tool::TEAM_CREATE_NAME,
                  R"({"goal":"goal","members":[{"name":"leader","instructions":"spoof","tools":[]}]})"},
        std::pair{tool::TEAM_CREATE_NAME, R"({"goal":"goal","members":[],"scope":"other"})"},
        std::pair{tool::TEAM_MEMBER_ADD_NAME,
                  R"({"name":"custom","instructions":"inspect","tools":["FileRead","FileRead"]})"},
        std::pair{tool::TEAM_TASK_CREATE_NAME, R"({"key":"a","member":"one","prompt":"inspect","depends_on":["a"]})"},
        std::pair{tool::TEAM_TASK_UPDATE_NAME, R"({"key":"a","action":"accept","feedback":"checked"})"},
        std::pair{tool::TEAM_TASK_UPDATE_NAME, R"({"key":"a","attempt":1,"action":"accept"})"},
        std::pair{tool::TEAM_TASK_UPDATE_NAME, R"({"key":"a","attempt":4,"action":"revise","feedback":"fix"})"},
        std::pair{tool::TEAM_TASK_UPDATE_NAME, R"({"key":"a","attempt":1,"action":"finished","feedback":"checked"})"},
        std::pair{tool::TEAM_MESSAGE_NAME,
                  R"({"recipient":"one","task_key":"a","message":"approve","sender":"leader"})"},
        std::pair{tool::TEAM_GET_NAME, R"({"key":"a","offset":-1})"},
        std::pair{tool::TEAM_GET_NAME, R"({"key":"a","max_bytes":8193})"},
        std::pair{tool::TEAM_STOP_NAME, R"({"team_id":"foreign"})"}};
    for (const auto& [name, input] : invalid) {
      auto result = co_await registry.dispatch(name, input, context);
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
    }
    CHECK(called == 0);

    auto snapshot = tool::DispatchContext::for_now(context, false);
    const std::array valid{
        std::pair{
            tool::TEAM_CREATE_NAME,
            R"({"goal":"goal","members":[{"name":"协议考古","instructions":"trace protocol history","tools":[]}]})"},
        std::pair{tool::TEAM_MEMBER_ADD_NAME,
                  R"({"name":"edge-cases","instructions":"find boundary cases","tools":["FileRead"]})"},
        std::pair{tool::TEAM_TASK_CREATE_NAME, R"({"key":"a","member":"one","prompt":"inspect"})"},
        std::pair{
            tool::TEAM_TASK_UPDATE_NAME,
            R"({"team_id":"00000000000000000000000000000001","key":"a","attempt":1,"action":"accept","feedback":"checked evidence"})"},
        std::pair{tool::TEAM_MESSAGE_NAME, R"({"recipient":"one","task_key":"a","message":"API uses UTC"})"},
        std::pair{tool::TEAM_GET_NAME, "{}"},
        std::pair{tool::TEAM_STOP_NAME, "{}"}};
    for (const auto& [name, input] : valid) {
      auto result = co_await registry.dispatch(name, input, snapshot);
      REQUIRE(result.has_value());
      CHECK(audit.events().back().reason == "runtime_tool");
    }
    CHECK(called == valid.size());
    context.team = {};
    auto disabled = co_await registry.dispatch(tool::TEAM_GET_NAME, "{}", context);
    REQUIRE_FALSE(disabled.has_value());
    CHECK(disabled.error().kind() == core::ErrorKind::permission_denied);
  });
}
