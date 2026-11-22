#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/hook.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] hook::ToolBeforePayload sample_before() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

[[nodiscard]] hook::ToolBeforePayload sample_before_with_redacted_input() {
  return hook::ToolBeforePayload{
      .tool_name = "FileEdit",
      .input_json = R"({"path":"notes.md","old_string":"secret","new_string":"public"})",
      .redacted_input_json =
          R"({"kind":"redacted_tool_input","input_hash":"abc","old_string_bytes":6,"new_string_bytes":6})",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

[[nodiscard]] hook::MemoryWritePayload sample_memory_write() {
  return hook::MemoryWritePayload{
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .record =
          hook::MemoryRecordPayload{
              .id = "memory-1",
              .scope_key = "scope",
              .kind = "project",
              .title = "Sensitive title",
              .body = "Sensitive body",
              .created_at = core::Time::epoch(),
              .updated_at = core::Time::epoch(),
              .last_read_at = core::Time::epoch(),
              .importance = 0.75,
              .tags = {"secret", "project"},
              .linked_record_ids = {"linked-1"},
              .shadow = false,
          },
      .redacted_record =
          hook::RedactedMemoryRecordPayload{
              .id = "memory-1",
              .scope_key = "scope",
              .kind = "project",
              .title_bytes = std::string_view{"Sensitive title"}.size(),
              .body_bytes = std::string_view{"Sensitive body"}.size(),
              .tag_count = 2,
              .linked_record_count = 1,
              .shadow = false,
          },
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

[[nodiscard]] hook::HookDecision decision_of(hook::HookDecisionKind kind, std::string reason = {}) {
  return hook::HookDecision{.kind = kind, .reason = std::move(reason)};
}

/// Gate that counts its calls and returns a fixed decision.
[[nodiscard]] hook::Sink deciding(std::string id, hook::HookDecision decision, std::size_t& calls) {
  return hook::Sink{
      .id = std::move(id),
      .decide = [decision = std::move(decision),
                 &calls](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<hook::HookDecision>> {
        ++calls;
        co_return decision;
      },
  };
}

[[nodiscard]] hook::Sink deciding(std::string id, hook::HookDecision decision) {
  return hook::Sink{
      .id = std::move(id),
      .decide = [decision = std::move(decision)](hook::Event, hook::PayloadPtr)
          -> async::Awaitable<core::Result<hook::HookDecision>> { co_return decision; },
  };
}

/// Gate that proceeds after `delay` unless cancelled first.
[[nodiscard]] hook::Sink slow(std::string id, std::chrono::milliseconds delay) {
  return hook::Sink{
      .id = std::move(id),
      .decide = [delay](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<hook::HookDecision>> {
        auto slept = co_await async::sleep_for(co_await asio::this_coro::executor, delay);
        if (!slept) {
          co_return std::unexpected(std::move(slept).error());
        }
        co_return hook::HookDecision{};
      },
  };
}

/// Gate that copies the delivered payload and proceeds.
template <class T>
[[nodiscard]] hook::Sink copying(std::string id, T& copy, bool trusted_local = false) {
  return hook::Sink{
      .id = std::move(id),
      .decide = [&copy](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
        copy = std::get<T>(*payload);
        co_return hook::HookDecision{};
      },
      .trusted_local = trusted_local,
  };
}

}  // namespace

TEST_CASE("publish_blocking on empty bus returns proceed", "[hook][bus][blocking]") {
  hook::Bus bus;
  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->reason.empty());
    REQUIRE(result->trace.empty());
  });
}

TEST_CASE("publish_blocking skips observe-only sinks", "[hook][bus][blocking]") {
  hook::Bus bus;
  bus.subscribe(hook::Sink{.id = "observer",
                           .observe = [](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> { co_return; }},
                {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace.empty());
  });
}

TEST_CASE("publish_blocking returns a single gate's decision", "[hook][bus][blocking]") {
  auto rewrite = decision_of(hook::HookDecisionKind::rewrite, "narrow_path");
  rewrite.rewritten_input_json = std::string{R"({"path":"src/main.cpp"})"};
  const auto decision = GENERATE_COPY(decision_of(hook::HookDecisionKind::veto, "policy"),
                                      rewrite,
                                      decision_of(hook::HookDecisionKind::require_approval, "operator_review"));
  hook::Bus bus;
  std::size_t calls = 0;
  bus.subscribe(deciding("gate", decision, calls), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == decision.kind);
    REQUIRE(result->reason == decision.reason);
    REQUIRE(result->rewritten_input_json == decision.rewritten_input_json);
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "gate");
    REQUIRE(result->trace[0].kind == decision.kind);
    REQUIRE(result->trace[0].reason == decision.reason);
  });

  REQUIRE(calls == 1);
}

TEST_CASE("publish_blocking short-circuits at the first non-proceed gate", "[hook][bus][blocking]") {
  hook::Bus bus;
  std::size_t first = 0;
  std::size_t second = 0;
  std::size_t third = 0;
  bus.subscribe(deciding("first", {}, first), {hook::Event::tool_before});
  bus.subscribe(deciding("second", decision_of(hook::HookDecisionKind::veto, "second-veto"), second),
                {hook::Event::tool_before});
  bus.subscribe(deciding("third", {}, third), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason == "second-veto");
    REQUIRE(result->trace.size() == 2);
    REQUIRE(result->trace[0].sink_id == "first");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace[1].sink_id == "second");
  });

  REQUIRE(first == 1);
  REQUIRE(second == 1);
  REQUIRE(third == 0);
}

TEST_CASE("publish_blocking returns proceed with every consulted gate traced", "[hook][bus][blocking]") {
  hook::Bus bus;
  bus.subscribe(deciding("first", {}), {hook::Event::tool_before});
  bus.subscribe(deciding("second", {}), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace.size() == 2);
    REQUIRE(result->trace[0].sink_id == "first");
    REQUIRE(result->trace[1].sink_id == "second");
  });
}

TEST_CASE("gate errors and exceptions veto with reason=hook_error", "[hook][bus][blocking]") {
  const bool throws = GENERATE(false, true);
  hook::Bus bus;
  std::size_t later = 0;
  bus.subscribe(hook::Sink{.id = "failing",
                           .decide = [throws](hook::Event,
                                              hook::PayloadPtr) -> async::Awaitable<core::Result<hook::HookDecision>> {
                             if (throws) {
                               throw std::runtime_error{"boom"};
                             }
                             co_return std::unexpected(core::Error::internal("boom"));
                           }},
                {hook::Event::tool_before});
  bus.subscribe(deciding("later", {}, later), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason == "hook_error: boom [sink=failing]");
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "failing");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
  });

  REQUIRE(later == 0);
}

TEST_CASE("gate timeout becomes veto with elapsed trace", "[hook][bus][blocking]") {
  hook::Bus bus{hook::BusOptions{.blocking_timeout = 5ms}};
  std::size_t later = 0;
  bus.subscribe(slow("slow", 1s), {hook::Event::tool_before});
  bus.subscribe(deciding("later", {}, later), {hook::Event::tool_before});

  test::run_async(
      [&](asio::io_context& /*io*/) -> async::Awaitable<void> {
        auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
        REQUIRE(result.has_value());
        REQUIRE(result->kind == hook::HookDecisionKind::veto);
        REQUIRE(result->reason == "hook_timeout");
        REQUIRE(result->trace.size() == 1);
        REQUIRE(result->trace[0].sink_id == "slow");
        REQUIRE(result->trace[0].elapsed == 5ms);
      },
      250ms);

  REQUIRE(later == 0);
}

TEST_CASE("approval prompts wait for a decision beyond the gate timeout", "[hook][bus][blocking]") {
  hook::Bus bus{hook::BusOptions{.blocking_timeout = 5ms}};
  bus.subscribe(slow("operator", 50ms), {hook::Event::permission_ask_rendered});

  test::run_async(
      [&](asio::io_context& /*io*/) -> async::Awaitable<void> {
        auto result = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(
            hook::PermissionAskRenderedPayload{.tool_name = "FileWrite", .decision_reason = "operator_review"});
        REQUIRE(result.has_value());
        REQUIRE(result->kind == hook::HookDecisionKind::proceed);
        REQUIRE(result->trace.size() == 1);
        REQUIRE_FALSE(result->trace[0].elapsed.has_value());
      },
      1s);
}

TEST_CASE("publish_blocking redacts input_json for untrusted gates", "[hook][bus][blocking][redaction]") {
  hook::Bus bus;
  hook::ToolBeforePayload redacted;
  hook::ToolBeforePayload original;
  bus.subscribe(copying("default", redacted), {hook::Event::tool_before});
  bus.subscribe(copying("trusted", original, true), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before_with_redacted_input());
    REQUIRE(result.has_value());
    REQUIRE(result->trace.size() == 2);
  });

  REQUIRE(redacted.input_json ==
          R"({"kind":"redacted_tool_input","input_hash":"abc","old_string_bytes":6,"new_string_bytes":6})");
  REQUIRE(original.input_json == R"({"path":"notes.md","old_string":"secret","new_string":"public"})");
}

TEST_CASE("publish_blocking redacts memory write records for untrusted gates",
          "[hook][bus][blocking][redaction][memory]") {
  hook::Bus bus;
  hook::MemoryWritePayload redacted;
  hook::MemoryWritePayload original;
  bus.subscribe(copying("default", redacted), {hook::Event::memory_write_before});
  bus.subscribe(copying("trusted", original, true), {hook::Event::memory_write_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::memory_write_before>(sample_memory_write());
    REQUIRE(result.has_value());
    REQUIRE(result->trace.size() == 2);
  });

  REQUIRE(redacted.record.id == "memory-1");
  REQUIRE(redacted.record.kind == "project");
  REQUIRE(redacted.record.title.empty());
  REQUIRE(redacted.record.body.empty());
  REQUIRE(redacted.record.tags.empty());
  REQUIRE(redacted.record.linked_record_ids.empty());
  REQUIRE(redacted.redacted_record->title_bytes == std::string_view{"Sensitive title"}.size());
  REQUIRE(redacted.redacted_record->body_bytes == std::string_view{"Sensitive body"}.size());
  REQUIRE(redacted.redacted_record->tag_count == 2);
  REQUIRE(redacted.redacted_record->linked_record_count == 1);

  REQUIRE(original.record.title == "Sensitive title");
  REQUIRE(original.record.body == "Sensitive body");
  REQUIRE(original.record.tags == std::vector<std::string>{"secret", "project"});
  REQUIRE(original.record.linked_record_ids == std::vector<std::string>{"linked-1"});
}

TEST_CASE("only effect gates admit blocking publication", "[hook][event][blocking]") {
  STATIC_REQUIRE(hook::is_gate(hook::Event::tool_before));
  STATIC_REQUIRE(hook::is_gate(hook::Event::permission_ask_rendered));
  STATIC_REQUIRE(hook::is_gate(hook::Event::memory_write_before));
  STATIC_REQUIRE_FALSE(hook::is_gate(hook::Event::tool_after));
  STATIC_REQUIRE_FALSE(hook::is_gate(hook::Event::memory_read_after));
  STATIC_REQUIRE_FALSE(hook::is_gate(hook::Event::provider_response));
}
