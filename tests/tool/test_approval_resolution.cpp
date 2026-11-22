// tests/tool/test_approval_resolution.cpp — direct approval state-machine tests.

#include "../../src/oran-tool/_impl/approval_resolution.hpp"

#include <chrono>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/sink.hpp>
#include <oran/permission/approval_broker.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace detail = orangutan::tool::detail;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace test = orangutan::tests;

namespace {

[[nodiscard]] hook::Sink deciding(hook::HookDecision decision) {
  return hook::Sink{
      .id = "approval-test",
      .decide = [decision = std::move(decision)](hook::Event, hook::PayloadPtr)
          -> async::Awaitable<core::Result<hook::HookDecision>> { co_return decision; },
  };
}

[[nodiscard]] permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

[[nodiscard]] core::Time fixed_now() {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

[[nodiscard]] permission::Decision ask_decision() {
  return permission::Decision{
      .verdict = permission::Verdict::ask,
      .reason = "approval test",
      .replay_max = 3,
      .approval_ttl = std::chrono::seconds{120},
  };
}

[[nodiscard]] hook::PermissionAskRenderedPayload payload(core::Time now) {
  return hook::PermissionAskRenderedPayload{
      .tool_name = "Demo",
      .input_json = R"({"x":1})",
      .who = hook::Identity{.scope_key = "scope-A", .agent_key = "coder", .identity = "operator-1"},
      .decision_reason = "approval test",
      .replay_max = 3,
      .approval_ttl = std::chrono::seconds{120},
      .requested_at = now,
  };
}

[[nodiscard]] bool has_reason(const core::Error& error, std::string_view reason) {
  return std::ranges::any_of(error.context(),
                             [reason](const auto& entry) { return entry.first == "reason" && entry.second == reason; });
}

}  // namespace

TEST_CASE("approval resolution accepts a valid replay token", "[unit][tool][approval_resolution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto broker = make_broker();
    const auto now = fixed_now();
    const auto decision = ask_decision();
    const auto token = broker.approve(
        permission::ApprovalGrant{
            .tool_name = "Demo",
            .input = R"({"x":1})",
            .identity = "operator-1",
            .ttl = decision.approval_ttl,
            .replay_max = decision.replay_max,
        },
        now);

    auto resolved = co_await detail::resolve_ask(detail::ApprovalRequest{
        .tool_name = "Demo",
        .input = R"({"x":1})",
        .identity = "operator-1",
        .decision = decision,
        .now = now,
        .broker = &broker,
        .token = &token,
        .payload = payload(now),
    });

    REQUIRE(resolved.state == detail::ApprovalState::approved);
    REQUIRE_FALSE(resolved.rejection.has_value());
  });
}

TEST_CASE("approval resolution exposes broker rejection reasons", "[unit][tool][approval_resolution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto broker = make_broker();
    const auto now = fixed_now();
    const auto decision = ask_decision();
    const auto token = broker.approve(
        permission::ApprovalGrant{
            .tool_name = "Demo",
            .input = R"({"x":1})",
            .identity = "operator-1",
            .ttl = decision.approval_ttl,
            .replay_max = 0,
        },
        now);

    auto resolved = co_await detail::resolve_ask(detail::ApprovalRequest{
        .tool_name = "Demo",
        .input = R"({"x":1})",
        .identity = "operator-1",
        .decision = decision,
        .now = now,
        .broker = &broker,
        .token = &token,
        .payload = payload(now),
    });

    REQUIRE(resolved.state == detail::ApprovalState::rejected);
    REQUIRE(resolved.rejection.has_value());
    REQUIRE(has_reason(*resolved.rejection, "replay_exhausted"));
    REQUIRE(resolved.audit_reason == "replay_exhausted");
  });
}

TEST_CASE("approval resolution remains pending without a token or prompt sink", "[unit][tool][approval_resolution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto broker = make_broker();
    const auto now = fixed_now();
    const auto decision = ask_decision();

    auto resolved = co_await detail::resolve_ask(detail::ApprovalRequest{
        .tool_name = "Demo",
        .input = R"({"x":1})",
        .identity = "operator-1",
        .decision = decision,
        .now = now,
        .broker = &broker,
        .payload = payload(now),
    });

    REQUIRE(resolved.state == detail::ApprovalState::pending);
  });
}

TEST_CASE("approval resolution issues a replay token after operator approval", "[unit][tool][approval_resolution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto broker = make_broker();
    const auto now = fixed_now();
    const auto decision = ask_decision();
    auto operator_decision = hook::HookDecision{};
    operator_decision.reason = "operator_approved:operator-1";
    hook::Bus bus;
    bus.subscribe(deciding(operator_decision), {hook::Event::permission_ask_rendered});
    auto token = permission::ApprovalToken{};

    auto resolved = co_await detail::resolve_ask(detail::ApprovalRequest{
        .tool_name = "Demo",
        .input = R"({"x":1})",
        .identity = "operator-1",
        .decision = decision,
        .now = now,
        .broker = &broker,
        .token_output = &token,
        .bus = &bus,
        .payload = payload(now),
    });

    REQUIRE(resolved.state == detail::ApprovalState::approved);
    REQUIRE(resolved.trace.size() == 1);
    REQUIRE(token.tool_name == "Demo");
  });
}

TEST_CASE("approval resolution rejects proceed without operator identity", "[unit][tool][approval_resolution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    auto broker = make_broker();
    const auto now = fixed_now();
    const auto decision = ask_decision();
    hook::Bus bus;
    bus.subscribe(deciding(hook::HookDecision{}), {hook::Event::permission_ask_rendered});

    auto resolved = co_await detail::resolve_ask(detail::ApprovalRequest{
        .tool_name = "Demo",
        .input = R"({"x":1})",
        .identity = "operator-1",
        .decision = decision,
        .now = now,
        .broker = &broker,
        .bus = &bus,
        .payload = payload(now),
    });

    REQUIRE(resolved.state == detail::ApprovalState::rejected);
    REQUIRE(resolved.rejection.has_value());
    REQUIRE(has_reason(*resolved.rejection, "operator_denied"));
    REQUIRE(resolved.trace.size() == 1);
  });
}
