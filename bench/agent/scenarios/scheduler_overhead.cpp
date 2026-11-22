// bench/agent/scenarios/scheduler_overhead.cpp
//
// Compare the fixed cost of scheduled and direct dispatch under the same
// executor and permission setup. Interpret timings on the measured host.
//
//   A = `agent.scheduler_overhead_direct_dispatch` : one `Registry::dispatch`
//       of a no-op tool through an allow rule + `NullAuditSink`, no hook bus.
//   B = `agent.scheduler_overhead_run_batch`       : one-call
//       `ToolScheduler::run_batch` of the same tool (bounded call admission,
//       ordered-result drain, per-call timeout race, no path lock
//       because the tool declares no capabilities).
//
// B / A is the scheduler's per-batch overhead. Both paths share the same
// io_context-restart harness so the driver cost cancels in the ratio.

#include <nanobench.h>

#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

namespace orangutan::bench {
namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

void add_noop_tool(tool::Registry& registry) {
  auto def = core::ToolDef{
      .name = "BenchNoop",
      .description = "No-op bench tool",
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":true})",
      .required_capabilities = {},
  };
  auto handler = [](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    co_return tool::Output::text_only("ok");
  };
  if (!registry.add(std::move(def), std::move(handler))) {
    std::abort();
  }
}

permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.push_back(
      permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "*", .capability = std::nullopt});
  return rules;
}

tool::DispatchContext make_ctx(asio::io_context& io, permission::RuleSet& rules, permission::AuditSink& audit) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .scope_key = "bench",
      .agent_key = "bench",
      .identity = "bench",
  };
}

}  // namespace

void register_scheduler_overhead(ankerl::nanobench::Bench& bench) {
  asio::io_context io;
  tool::Registry registry;
  add_noop_tool(registry);
  auto rules = allow_all_rules();
  permission::NullAuditSink audit;
  agent::ToolScheduler scheduler{io.get_executor(), registry, agent::ToolSchedulerOptions{}};

  bench.run("agent.scheduler_overhead_direct_dispatch", [&] {
    io.restart();
    auto ctx = make_ctx(io, rules, audit);
    bool ok = false;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          auto out = co_await registry.dispatch("BenchNoop", "{}", ctx);
          ok = out.has_value();
        },
        asio::detached);
    io.run();
    if (!ok) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(ok);
  });

  bench.run("agent.scheduler_overhead_run_batch", [&] {
    io.restart();
    auto ctx = make_ctx(io, rules, audit);
    bool ok = false;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          std::vector<agent::ToolBatchCall> batch;
          batch.push_back(agent::ToolBatchCall{.tool_use_id = "b", .name = "BenchNoop", .input_json = "{}"});
          auto out = co_await scheduler.run_batch(std::move(batch), ctx);
          ok = out.has_value() && out->size() == 1 && (*out)[0].output.has_value();
        },
        asio::detached);
    io.run();
    if (!ok) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(ok);
  });
}

}  // namespace orangutan::bench
