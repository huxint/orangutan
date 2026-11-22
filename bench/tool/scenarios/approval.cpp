// bench/tool/scenarios/approval.cpp
//
// Slice 21 — approval-broker dispatch overhead.
//
// Three-way comparison of the `Verdict::ask` paths in `Registry::dispatch`:
//
//   A. `dispatch_ask_short_circuit` — broker pointer is null. Same cost as
//      slice-20's behavior: rule evaluation + audit record + `permission_denied`
//      build with the new `replay_max` / `approval_ttl_seconds` context
//      entries. The baseline a caller pays when no operator approval is on
//      hand yet.
//   B. `dispatch_ask_approved` — broker + token are supplied and the token
//      verifies. Adds the broker's HMAC verify + map lookup + decrement on
//      top of the (A) baseline, then runs the trivial in-memory handler.
//      This is the steady-state cost the agent loop pays for an already-
//      approved tool call during replay.
//   C. `dispatch_ask_rejected` — broker + token are supplied but the token
//      is exhausted (`replay_max=0`). Same broker cost as (B) on the
//      failure branch, but the handler is skipped.
//
// The contrast (B - A) is the broker-attached overhead an operator pays
// for "agent already has approval"; the contrast (C - A) is the rejection
// overhead a flaky/expired token costs. Both should be in the same order
// as `permission/scenarios/approval_broker.cpp`'s raw broker numbers (a
// few µs); the gap with the raw broker bench is the dispatch + audit
// surface around the call.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bench {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

[[nodiscard]] async::Awaitable<core::Result<tool::Output>> noop_handler(std::string_view /*input*/,
                                                                        tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{.text = "ok"};
}

[[nodiscard]] core::Time fixed_now() noexcept {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

[[gnu::noinline]] std::size_t run_dispatch(asio::io_context& io,
                                           tool::Registry& registry,
                                           tool::DispatchContext& ctx,
                                           std::string_view input,
                                           bool expect_success) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result = co_await registry.dispatch("noop", input, ctx);
        if (result.has_value() != expect_success) {
          std::abort();
        }
        counter += result ? result->text.size() : result.error().context().size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return counter;
}

}  // namespace

void register_tool_approval(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  if (!registry.add(core::ToolDef{.name = "noop",
                                  .description = "noop approval bench tool",
                                  .input_schema_json = "{}",
                                  .required_capabilities = {core::Capability::read_file}},
                    &noop_handler)) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::ask, .tool_pattern = "noop"});
  permission::RecordingAuditSink sink;

  auto broker_ok_result = permission::ApprovalBroker::with_random_secret();
  auto broker_bad_result = permission::ApprovalBroker::with_random_secret();
  if (!broker_ok_result || !broker_bad_result) {
    std::abort();
  }
  auto broker_ok = std::move(*broker_ok_result);
  auto broker_bad = std::move(*broker_bad_result);

  const auto now = fixed_now();
  const std::string input = R"({"path":"/tmp/x"})";
  // Effectively-infinite replay so the bench loop's repeated `check` calls
  // never trip the exhaustion branch and we measure the steady-state cost.
  const auto good_token =
      broker_ok.approve(permission::ApprovalGrant{.tool_name = "noop",
                                                  .input = input,
                                                  .identity = "operator-1",
                                                  .ttl = std::chrono::hours{24},
                                                  .replay_max = std::numeric_limits<std::uint32_t>::max()},
                        now);
  // `replay_max=0` rejects on every check while still hitting the map
  // lookup, so the rejected scenario pays the same broker work the
  // approved scenario does, minus the decrement and the handler.
  const auto bad_token = broker_bad.approve(permission::ApprovalGrant{.tool_name = "noop",
                                                                      .input = input,
                                                                      .identity = "operator-1",
                                                                      .ttl = std::chrono::hours{24},
                                                                      .replay_max = 0U},
                                            now);

  asio::io_context io;

  tool::DispatchContext short_circuit_ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .approval_broker = nullptr,
      .approval_token = nullptr,
      .now = now,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };
  tool::DispatchContext approved_ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .approval_broker = &broker_ok,
      .approval_token = &good_token,
      .now = now,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };
  tool::DispatchContext rejected_ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .approval_broker = &broker_bad,
      .approval_token = &bad_token,
      .now = now,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  bench.run("dispatch_ask_short_circuit", [&] {
    sink.clear();
    const auto value = run_dispatch(io, registry, short_circuit_ctx, input, /*expect_success=*/false);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("dispatch_ask_approved", [&] {
    sink.clear();
    const auto value = run_dispatch(io, registry, approved_ctx, input, /*expect_success=*/true);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("dispatch_ask_rejected", [&] {
    sink.clear();
    const auto value = run_dispatch(io, registry, rejected_ctx, input, /*expect_success=*/false);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
