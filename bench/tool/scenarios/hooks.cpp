// bench/tool/scenarios/hooks.cpp
//
// Slice 22 — `Registry::dispatch` overhead with hook bus attached.
//
// Three-way comparison of the `Verdict::allow` dispatch path under varying
// hook configurations:
//
//   A. `dispatch_allow_no_hooks` — `DispatchContext::bus == nullptr`. The
//      baseline a caller pays when no observer is attached. Matches the
//      slice-17 `registry.dispatch_allow` baseline.
//   B. `dispatch_allow_with_empty_bus` — `bus` non-null but no sink is
//      subscribed to `tool_before` / `tool_after`. Measures the cost of the
//      two `publish_advisory` no-op map lookups the dispatch pays for "bus
//      is attached but nothing listens".
//   C. `dispatch_allow_with_two_sinks` — bus + one callback sink subscribed
//      to both `tool_before` and `tool_after`. Measures the per-call cost
//      of two actual sink dispatches.
//
// The (B − A) delta is the "wired but unused" cost an operator pays for
// keeping a bus attached even when nothing is listening; (C − A) is the
// per-call cost of one observer subscribed to both bookend events. Both
// should be small relative to the audit-record cost (~18 µs StorageAuditSink)
// so a future deferred-publish or batched publish would not move the needle
// on the agent loop's critical path.

#include <nanobench.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bench {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

[[nodiscard]] async::Awaitable<core::Result<tool::Output>> noop_handler(std::string_view /*input*/,
                                                                        tool::DispatchContext& /*ctx*/) {
  co_return tool::Output{.text = "ok"};
}

[[gnu::noinline]] std::size_t drive_dispatch(asio::io_context& io,
                                             const tool::Registry& registry,
                                             tool::DispatchContext& ctx,
                                             std::string_view input) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result = co_await registry.dispatch("noop", input, ctx);
        if (!result.has_value()) {
          std::abort();
        }
        counter += result->text.size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return counter;
}

}  // namespace

void register_tool_hooks(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  if (!registry.add(core::ToolDef{.name = "noop",
                                  .description = "noop hook bench tool",
                                  .input_schema_json = "{}",
                                  .required_capabilities = {core::Capability::read_file}},
                    &noop_handler)) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
  permission::RecordingAuditSink sink;

  const std::string_view input{R"({"path":"/tmp/x"})"};

  asio::io_context io;

  tool::DispatchContext ctx_a{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .bus = nullptr,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  hook::Bus empty_bus;
  tool::DispatchContext ctx_b{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .bus = &empty_bus,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  hook::Bus busy_bus;
  std::size_t before_count = 0;
  std::size_t after_count = 0;
  hook::Sink before_sink{
      .id = "bench-before",
      .observe = [&before_count](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
        ++before_count;
        co_return;
      },
  };
  hook::Sink after_sink{
      .id = "bench-after",
      .observe = [&after_count](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
        ++after_count;
        co_return;
      },
  };
  busy_bus.subscribe(before_sink, {hook::Event::tool_before});
  busy_bus.subscribe(after_sink, {hook::Event::tool_after});

  tool::DispatchContext ctx_c{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .bus = &busy_bus,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  bench.run("dispatch_allow_no_hooks", [&] {
    sink.clear();
    const auto value = drive_dispatch(io, registry, ctx_a, input);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("dispatch_allow_with_empty_bus", [&] {
    sink.clear();
    const auto value = drive_dispatch(io, registry, ctx_b, input);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("dispatch_allow_with_two_sinks", [&] {
    sink.clear();
    const auto value = drive_dispatch(io, registry, ctx_c, input);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(before_count);
  ankerl::nanobench::doNotOptimizeAway(after_count);
}

}  // namespace orangutan::bench
