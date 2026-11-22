// bench/tool/scenarios/dispatch.cpp
//
// A-vs-B comparison: registry catalog lookup vs. a full `dispatch` walk that
// runs the rule evaluator, records a `RecordingAuditSink` event, and invokes
// a trivial in-memory handler. The contrast measures the dispatch overhead
// (permission walk + audit record + virtual dispatch into the handler) over
// the pure catalog lookup the agent loop also makes on every iteration.

#include <nanobench.h>

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

[[gnu::noinline]] std::size_t
run_dispatch(asio::io_context& io, tool::Registry& registry, tool::DispatchContext& ctx, std::string_view input) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result = co_await registry.dispatch("noop", input, ctx);
        if (!result) {
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

void register_tool_dispatch(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  auto add_result = registry.add(core::ToolDef{.name = "noop",
                                               .description = "noop bench tool",
                                               .input_schema_json = "{}",
                                               .required_capabilities = {core::Capability::read_file}},
                                 &noop_handler);
  if (!add_result) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "noop"});
  permission::RecordingAuditSink sink;

  asio::io_context io;
  tool::DispatchContext ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };
  const std::string input = R"({"path":"/tmp/x"})";

  bench.run("registry.lookup", [&] {
    const auto* def = registry.find("noop");
    ankerl::nanobench::doNotOptimizeAway(def);
  });
  bench.run("registry.dispatch_allow", [&] {
    sink.clear();
    const auto value = run_dispatch(io, registry, ctx, input);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
