// Compare eight concurrent audit writes with a no-op sink. An in-memory
// database isolates writer-lease and SQL cost from disk latency.

#include <nanobench.h>

#include <cstddef>
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
#include <oran/storage.hpp>
#include <oran/tool.hpp>

namespace orangutan::bench {
namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace storage = orangutan::storage;
namespace tool = orangutan::tool;

namespace {

constexpr std::size_t kBatchSize = 8;

void add_audited_tool(tool::Registry& registry) {
  auto def = core::ToolDef{
      .name = "BenchAudited",
      .description = "No-cap tool that records a decision row per call",
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

std::vector<agent::ToolBatchCall> make_batch() {
  std::vector<agent::ToolBatchCall> batch;
  batch.reserve(kBatchSize);
  for (std::size_t i = 0; i < kBatchSize; ++i) {
    batch.push_back(agent::ToolBatchCall{
        .tool_use_id = "call-" + std::to_string(i),
        .name = "BenchAudited",
        .input_json = "{}",
    });
  }
  return batch;
}

void run_batch_once(asio::io_context& io,
                    agent::ToolScheduler& scheduler,
                    permission::RuleSet& rules,
                    permission::AuditSink& audit) {
  io.restart();
  auto ctx = make_ctx(io, rules, audit);
  bool ok = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto out = co_await scheduler.run_batch(make_batch(), ctx);
        ok = out.has_value() && out->size() == kBatchSize;
      },
      asio::detached);
  io.run();
  if (!ok) {
    std::abort();
  }
  ankerl::nanobench::doNotOptimizeAway(ok);
}

}  // namespace

void register_scheduler_audit_fanout(ankerl::nanobench::Bench& bench) {
  auto rules = allow_all_rules();

  {
    asio::io_context io;
    tool::Registry registry;
    add_audited_tool(registry);
    permission::NullAuditSink audit;
    agent::ToolScheduler scheduler{io.get_executor(), registry, agent::ToolSchedulerOptions{}};
    bench.run("agent.scheduler_audit_fanout_null", [&] { run_batch_once(io, scheduler, rules, audit); });
  }

  {
    asio::io_context io;
    tool::Registry registry;
    add_audited_tool(registry);

    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = ":memory:", .reader_count = 1, .statement_cache_capacity = 16});
    if (!pool_result) {
      std::abort();
    }
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    bool migrated = false;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          auto result = co_await repo.migrate();
          migrated = result.has_value();
        },
        asio::detached);
    io.run();
    if (!migrated) {
      std::abort();
    }
    permission::StorageAuditSink audit{repo, io.get_executor()};
    agent::ToolScheduler scheduler{io.get_executor(), registry, agent::ToolSchedulerOptions{}};
    bench.run("agent.scheduler_audit_fanout_storage", [&] { run_batch_once(io, scheduler, rules, audit); });
  }
}

}  // namespace orangutan::bench
