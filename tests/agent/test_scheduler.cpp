#include <oran/agent.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool error_has_context(const core::Error& error, std::string_view key, std::string_view value) {
  for (const auto& [k, v] : error.context()) {
    if (k == key && v == value) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] core::ToolDef tool_def(std::string name) {
  return core::ToolDef{
      .name = std::move(name),
      .description = "Latency-controllable test tool",
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":true})",
      .required_capabilities = {},
  };
}

[[nodiscard]] permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  return rules;
}

struct ConcurrencyTracker {
  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};

  void enter() noexcept {
    const auto current = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    auto observed = peak.load(std::memory_order_relaxed);
    while (current > observed && !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {
      // peak races between observation and update; retry until either we
      // raise the bar or someone else already raised it past `current`.
    }
  }
  void leave() noexcept {
    in_flight.fetch_sub(1, std::memory_order_relaxed);
  }
};

/// Register a handler that sleeps for `latency` and returns `done:<input>`.
void add_latency_tool(tool::Registry& registry,
                      std::string name,
                      std::chrono::milliseconds latency,
                      std::shared_ptr<ConcurrencyTracker> tracker = {}) {
  auto handler = [latency, tracker = std::move(tracker)](
                     std::string_view input,
                     tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    if (tracker) {
      tracker->enter();
    }
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    if (tracker) {
      tracker->leave();
    }
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return tool::Output::text_only(std::string{"done:"} + std::string{input});
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a handler that sleeps for `latency` and then returns an
/// infrastructure error. Used to prove a failing call still records its audit
/// decision row and still emits `tool_after` (AC7's failure-path clause).
void add_failing_tool(tool::Registry& registry, std::string name, std::chrono::milliseconds latency) {
  auto handler = [latency](std::string_view,
                           tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return std::unexpected(core::Error::internal("fake tool: deliberate handler failure"));
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a handler that DELIBERATELY ignores its cancellation slot: it
/// disables cancellation for its own coroutine, then sleeps. Models the "tool
/// bug" spec 0012 AC5 carves out — a handler that never polls its cancellation
/// state. Under parent cancellation the scheduler's 100 ms guarantee must still
/// hold (`run_batch` returns promptly without waiting for this handler) and the
/// offending tool must be named in a `cancellation_lag` audit row.
void add_uncancellable_tool(tool::Registry& registry, std::string name, std::chrono::milliseconds latency) {
  auto handler = [latency](std::string_view,
                           tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    [[maybe_unused]] auto slept = co_await async::sleep_for(ctx.executor, latency);
    co_return tool::Output::text_only("done-uncancellable");
  };
  REQUIRE(registry.add(tool_def(std::move(name)), std::move(handler)).has_value());
}

/// Register a mutating handler whose completion is controlled by the test.
/// It deliberately ignores cancellation so the scheduler returns after its
/// grace window while the handler still owns a per-path lock guard.
void add_gated_uncancellable_write_tool(tool::Registry& registry,
                                        std::string name,
                                        std::shared_ptr<async::Channel<bool>> release_gate,
                                        std::shared_ptr<std::atomic<bool>> started,
                                        std::shared_ptr<std::atomic<bool>> finished) {
  auto handler = [release_gate = std::move(release_gate), started = std::move(started), finished = std::move(finished)](
                     std::string_view,
                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    started->store(true, std::memory_order_release);
    auto released = co_await release_gate->receive();
    if (!released) {
      co_return std::unexpected(released.error());
    }
    finished->store(true, std::memory_order_release);
    co_return tool::Output::text_only("done-uncancellable-write");
  };
  auto def = tool_def(std::move(name));
  def.required_capabilities = {core::Capability::write_file};
  REQUIRE(registry.add(std::move(def), std::move(handler)).has_value());
}

[[nodiscard]] permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

[[nodiscard]] permission::ApprovalToken
grant(permission::ApprovalBroker& broker, std::string_view tool_name, std::string_view input, core::Time now) {
  return broker.approve(
      permission::ApprovalGrant{
          .tool_name = tool_name,
          .input = input,
          .identity = "operator-1",
          .ttl = std::chrono::seconds{60},
          .replay_max = 4,
      },
      now);
}

[[nodiscard]] tool::DispatchContext
make_prototype(asio::io_context& io, permission::RuleSet& rules, permission::AuditSink& audit) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] agent::ToolBatchCall call(std::size_t index, std::string name, std::string input_json = "{}") {
  return agent::ToolBatchCall{
      .tool_use_id = std::string{"call-"} + std::to_string(index),
      .name = std::move(name),
      .input_json = std::move(input_json),
  };
}

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void touch_file(const std::filesystem::path& path) {
  std::ofstream out{path};
  REQUIRE(out.good());
}

struct Interval {
  std::chrono::steady_clock::time_point start{};
  std::chrono::steady_clock::time_point end{};
};

[[nodiscard]] bool overlaps(const Interval& a, const Interval& b) {
  return a.start < b.end && b.start < a.end;
}

struct LockTracker {
  std::mutex mu;
  std::vector<std::pair<std::string, Interval>> intervals;
  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};

  void enter() noexcept {
    const auto current = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    auto observed = peak.load(std::memory_order_relaxed);
    while (current > observed && !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {}
  }
  void leave() noexcept {
    in_flight.fetch_sub(1, std::memory_order_relaxed);
  }
  void record(std::string key, Interval iv) {
    std::scoped_lock l{mu};
    intervals.emplace_back(std::move(key), iv);
  }
  [[nodiscard]] std::vector<Interval> for_key(std::string_view key) {
    std::scoped_lock l{mu};
    std::vector<Interval> out;
    for (const auto& [k, iv] : intervals) {
      if (k == key) {
        out.push_back(iv);
      }
    }
    return out;
  }
};

[[nodiscard]] core::ToolDef tracked_tool_def(std::string name, std::vector<core::Capability> caps) {
  return core::ToolDef{
      .name = std::move(name),
      .description = "Lock-tracking fixture tool",
      .input_schema_json =
          R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":true})",
      .required_capabilities = std::move(caps),
  };
}

void add_tracked_tool(tool::Registry& registry,
                      std::string name,
                      std::vector<core::Capability> caps,
                      std::chrono::milliseconds latency,
                      std::shared_ptr<LockTracker> tracker) {
  auto handler = [latency, tracker = std::move(tracker)](
                     std::string_view input,
                     tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    tracker->enter();
    const auto start = std::chrono::steady_clock::now();
    auto sleep_result = co_await async::sleep_for(ctx.executor, latency);
    const auto end = std::chrono::steady_clock::now();
    tracker->leave();
    // Fake tools live outside the registry's hard-coded built-in name set,
    // so `ctx.resolved_path` is never populated for them. Track by the raw
    // input bytes — two calls targeting the same path share an input string,
    // which is what the tests assert on.
    tracker->record(std::string{input}, Interval{.start = start, .end = end});
    if (!sleep_result) {
      co_return std::unexpected(sleep_result.error());
    }
    co_return tool::Output::text_only(std::string{"done"});
  };
  REQUIRE(registry.add(tracked_tool_def(std::move(name), std::move(caps)), std::move(handler)).has_value());
}

[[nodiscard]] tool::Workspace make_workspace(const std::filesystem::path& root) {
  auto workspace = tool::Workspace::create(root.string());
  REQUIRE(workspace.has_value());
  return std::move(*workspace);
}

[[nodiscard]] tool::DispatchContext make_prototype_with_workspace(asio::io_context& io,
                                                                  permission::RuleSet& rules,
                                                                  permission::AuditSink& audit,
                                                                  tool::Workspace& workspace) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = audit,
      .workspace = &workspace,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] std::string write_call_input(std::string_view rel_path) {
  std::string out;
  out.reserve(rel_path.size() + 20);
  out += R"({"path":")";
  out.append(rel_path);
  out += R"("})";
  return out;
}

}  // namespace

TEST_CASE("ToolScheduler: empty batch returns an empty result vector", "[unit][agent][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto prototype = make_prototype(io, rules, audit);

    agent::ToolScheduler scheduler{io.get_executor(), registry};
    auto result = co_await scheduler.run_batch({}, prototype);

    REQUIRE(result.has_value());
    REQUIRE(result->empty());
  });
}

TEST_CASE("ToolScheduler: single call returns one ordered result with the registry output",
          "[unit][agent][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    add_latency_tool(registry, "FakeNoop", 0ms);
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto prototype = make_prototype(io, rules, audit);

    agent::ToolScheduler scheduler{io.get_executor(), registry};

    std::vector<agent::ToolBatchCall> batch;
    batch.push_back(call(0, "FakeNoop", R"({"k":"v"})"));

    auto result = co_await scheduler.run_batch(std::move(batch), prototype);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].tool_use_id == "call-0");
    REQUIRE((*result)[0].name == "FakeNoop");
    REQUIRE((*result)[0].output.has_value());
    REQUIRE((*result)[0].output->text == R"(done:{"k":"v"})");
  });
}

TEST_CASE("ToolScheduler: bounded parallelism caps in-flight calls", "[unit][agent][scheduler][parallelism]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        constexpr std::size_t kBatch = 10;
        constexpr std::size_t kMaxParallel = 4;
        constexpr auto kLatency = 50ms;

        auto tracker = std::make_shared<ConcurrencyTracker>();
        tool::Registry registry;
        add_latency_tool(registry, "FakeLatency", kLatency, tracker);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{
                                           .max_parallel_tools = kMaxParallel,
                                           .per_call_timeout = 30s,
                                       }};

        std::vector<agent::ToolBatchCall> batch;
        batch.reserve(kBatch);
        for (std::size_t i = 0; i < kBatch; ++i) {
          batch.push_back(call(i, "FakeLatency"));
        }

        const auto started = std::chrono::steady_clock::now();
        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

        REQUIRE(result.has_value());
        REQUIRE(result->size() == kBatch);
        REQUIRE(tracker->peak.load() <= static_cast<int>(kMaxParallel));
        REQUIRE(tracker->peak.load() > 0);
        REQUIRE(elapsed >= 3 * kLatency / 2);  // AC1 lower bound: ceil(10/4) * 50ms = 150 ms.
        // Upper bound is lenient to absorb CI scheduler noise on WSL2.
        REQUIRE(elapsed <= 10 * kLatency);
      },
      2s);
}

TEST_CASE("ToolScheduler: results are returned in original tool_use order", "[unit][agent][scheduler][ordering]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "FakeSlow", 60ms);
        add_latency_tool(registry, "FakeFast", 5ms);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 2, .per_call_timeout = 5s}};

        const std::vector<std::string> inputs{
            R"({"value":"short"})", "{\"value\":\"" + std::string(4096, 'a') + "\"}",
            R"({"value":"queued"})", "{\"value\":\"" + std::string(8192, 'b') + "\"}"};
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeSlow", inputs[0]));
        batch.push_back(call(1, "FakeFast", inputs[1]));
        batch.push_back(call(2, "FakeSlow", inputs[2]));
        batch.push_back(call(3, "FakeFast", inputs[3]));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 4);
        REQUIRE((*result)[0].tool_use_id == "call-0");
        REQUIRE((*result)[0].name == "FakeSlow");
        REQUIRE((*result)[1].tool_use_id == "call-1");
        REQUIRE((*result)[1].name == "FakeFast");
        REQUIRE((*result)[2].tool_use_id == "call-2");
        REQUIRE((*result)[2].name == "FakeSlow");
        REQUIRE((*result)[3].tool_use_id == "call-3");
        REQUIRE((*result)[3].name == "FakeFast");
        // Queued and active inputs survive suspension and remain paired with
        // their call identities despite out-of-order completion.
        for (std::size_t i = 0; i < result->size(); ++i) {
          REQUIRE((*result)[i].output.has_value());
          REQUIRE((*result)[i].output->text == "done:" + inputs[i]);
        }
      },
      2s);
}

TEST_CASE("ToolScheduler: per-call timeout surfaces Error::cancelled with reason=timeout",
          "[unit][agent][scheduler][timeout]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "FakeSlow", 500ms);
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype(io, rules, audit);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{
                                           .max_parallel_tools = 4,
                                           .per_call_timeout = 50ms,
                                       }};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeSlow"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 1);
        const auto& row = (*result)[0];
        REQUIRE(row.tool_use_id == "call-0");
        REQUIRE_FALSE(row.output.has_value());
        REQUIRE(row.output.error().kind() == core::ErrorKind::cancelled);
        REQUIRE(error_has_context(row.output.error(), "reason", "timeout"));
        REQUIRE(error_has_context(row.output.error(), "tool", "FakeSlow"));
        REQUIRE(error_has_context(row.output.error(), "per_call_timeout_ms", "50"));
      },
      2s);
}

TEST_CASE("ToolScheduler: parent cancellation propagates to in-flight calls",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "FakeSlow", 1s);
  auto rules = allow_all_rules();
  permission::NullAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{
                                     .max_parallel_tools = 4,
                                     .per_call_timeout = 30s,
                                 }};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "FakeSlow"));
  batch.push_back(call(1, "FakeSlow"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  // Let the scheduler spawn its children before we cancel the parent.
  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));
}

TEST_CASE("ToolScheduler: two writes to the same canonical path serialize (AC3)", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-ac3"};
        const auto rel = std::string{"target.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "FakeLockWrite", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeLockWrite", write_call_input(rel)));
        batch.push_back(call(1, "FakeLockWrite", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[1].output.has_value());

        const auto intervals = tracker->for_key(write_call_input(rel));
        REQUIRE(intervals.size() == 2);
        REQUIRE_FALSE(overlaps(intervals[0], intervals[1]));
        REQUIRE(tracker->peak.load() == 1);
      },
      5s);
}

TEST_CASE("ToolScheduler: concurrent read and write on the same path obey the read/write lock (AC4)",
          "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-ac4"};
        const auto rel = std::string{"shared.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "FakeLockRead", {core::Capability::read_file}, 80ms, tracker);
        add_tracked_tool(registry, "FakeLockWrite", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeLockRead", write_call_input(rel)));
        batch.push_back(call(1, "FakeLockWrite", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[1].output.has_value());

        const auto intervals = tracker->for_key(write_call_input(rel));
        REQUIRE(intervals.size() == 2);
        REQUIRE_FALSE(overlaps(intervals[0], intervals[1]));
        REQUIRE(tracker->peak.load() == 1);
      },
      5s);
}

TEST_CASE("ToolScheduler: two reads to the same path run concurrently under the shared lock",
          "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-shared"};
        const auto rel = std::string{"shared-read.txt"};
        touch_file(root.path() / rel);

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "FakeLockRead", {core::Capability::read_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeLockRead", write_call_input(rel)));
        batch.push_back(call(1, "FakeLockRead", write_call_input(rel)));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE(tracker->peak.load() == 2);
      },
      5s);
}

TEST_CASE("ToolScheduler: writes to different paths run concurrently", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-distinct"};
        touch_file(root.path() / "a.txt");
        touch_file(root.path() / "b.txt");

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        add_tracked_tool(registry, "FakeLockWrite", {core::Capability::write_file}, 80ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeLockWrite", write_call_input("a.txt")));
        batch.push_back(call(1, "FakeLockWrite", write_call_input("b.txt")));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        REQUIRE(tracker->peak.load() == 2);
      },
      5s);
}

TEST_CASE("ToolScheduler: capability-free tools skip the path lock", "[unit][agent][scheduler][lock]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        TempDir root{"oran-sched-nocap"};
        touch_file(root.path() / "c.txt");

        auto tracker = std::make_shared<LockTracker>();
        tool::Registry registry;
        // No capability => no lock class, no path resolution required.
        add_tracked_tool(registry, "FakeLockNoop", {}, 30ms, tracker);

        auto workspace = make_workspace(root.path());
        auto rules = allow_all_rules();
        permission::NullAuditSink audit;
        auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 5s}};

        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeLockNoop", write_call_input("c.txt")));
        batch.push_back(call(1, "FakeLockNoop", write_call_input("c.txt")));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(tracker->peak.load() == 2);
      },
      5s);
}

TEST_CASE("ToolScheduler: a batch records exactly N audit rows and N tool_after publishes (AC7)",
          "[unit][agent][scheduler][audit]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "FakeOk", 40ms);
        add_failing_tool(registry, "FakeFail", 10ms);
        auto rules = allow_all_rules();
        permission::RecordingAuditSink audit;

        // Count `tool_after` publishes through an advisory in-process sink.
        auto after_publishes = std::make_shared<std::atomic<int>>(0);
        hook::Sink after_sink{
            .id = "after-counter",
            .observe = [after_publishes](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
              after_publishes->fetch_add(1, std::memory_order_relaxed);
              co_return;
            },
        };
        hook::Bus bus;
        bus.subscribe(after_sink, {hook::Event::tool_after});

        auto prototype = make_prototype(io, rules, audit);
        prototype.bus = &bus;

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        // Mixed success/failure with varied latency so completions interleave
        // out of original order — AC7 must hold "regardless of execution order".
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeOk"));
        batch.push_back(call(1, "FakeFail"));
        batch.push_back(call(2, "FakeOk"));
        batch.push_back(call(3, "FakeFail"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 4);

        // Exactly N permission-decision rows, one per dispatch.
        REQUIRE(audit.events().size() == 4);
        for (const auto& event : audit.events()) {
          REQUIRE(event.event_kind == "permission_decision");
        }

        // Exactly N `tool_after` publishes, including the two failing calls.
        REQUIRE(after_publishes->load() == 4);

        // Ordered results preserve the per-call success/failure pattern.
        REQUIRE((*result)[0].output.has_value());
        REQUIRE_FALSE((*result)[1].output.has_value());
        REQUIRE((*result)[2].output.has_value());
        REQUIRE_FALSE((*result)[3].output.has_value());
      },
      5s);
}

TEST_CASE("ToolScheduler: ask calls resolve per call and a denied call is not hidden",
          "[unit][agent][scheduler][approval]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        tool::Registry registry;
        add_latency_tool(registry, "FakeAsk", 20ms);
        permission::RuleSet rules;
        rules.push_back(permission::Rule{
            .verdict = permission::Verdict::ask,
            .tool_pattern = "FakeAsk",
            .capability = std::nullopt,
        });
        permission::RecordingAuditSink audit;

        // The broker holds a grant for one specific input. The scheduler
        // refreshes `now` to the wall clock per call, so grant at the real
        // clock keeps the 60 s TTL valid through dispatch.
        auto broker = make_broker();
        const auto now = core::time::now_utc();
        constexpr std::string_view approved_input = R"({"k":"approve"})";
        const auto token = grant(broker, "FakeAsk", approved_input, now);

        auto prototype = make_prototype(io, rules, audit);
        prototype.approval_broker = &broker;
        prototype.approval_token = &token;

        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 10s}};

        // call 0 matches the broker grant -> approved -> handler runs.
        // call 1 carries a different input the grant does not cover -> rejected.
        // Both are `ask`; each must resolve through the broker on its own slot.
        std::vector<agent::ToolBatchCall> batch;
        batch.push_back(call(0, "FakeAsk", std::string{approved_input}));
        batch.push_back(call(1, "FakeAsk", R"({"k":"unauthorized"})"));

        auto result = co_await scheduler.run_batch(std::move(batch), prototype);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);

        // Ordered results: the approved call ran; the unauthorized call surfaces
        // as permission_denied at its own index rather than being hidden behind
        // the successful call.
        REQUIRE((*result)[0].output.has_value());
        REQUIRE((*result)[0].output->text == R"(done:{"k":"approve"})");
        REQUIRE_FALSE((*result)[1].output.has_value());
        REQUIRE((*result)[1].output.error().kind() == core::ErrorKind::permission_denied);
        REQUIRE(error_has_context((*result)[1].output.error(), "tool", "FakeAsk"));

        // Each call recorded exactly one decision row carrying its own outcome.
        REQUIRE(audit.events().size() == 2);
        const auto approved = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
          return e.outcome == permission::AuditOutcome::approved;
        });
        const auto rejected = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
          return e.outcome == permission::AuditOutcome::rejected;
        });
        REQUIRE(approved == 1);
        REQUIRE(rejected == 1);
      },
      5s);
}

// ---------------------------------------------------------------------------
// Slice 119 — cancellation propagation + `cancellation_lag` audit (AC5). A
// parent cancel must end every *cancel-aware* in-flight call within the 100 ms
// guarantee, and `run_batch` must return promptly even when a tool handler
// ignores its cancellation slot. Such a handler cannot be forced to stop
// (asio cancellation is cooperative; `co_await (dispatch || timeout)` will not
// resolve until the ignoring handler finishes), so the scheduler stops
// *awaiting* it: after the grace window it names the offending tool in a
// `cancellation_lag` audit row and returns, leaving the laggard to wind down
// on its own. A batch of purely cancel-aware tools records no such row.
// ---------------------------------------------------------------------------

TEST_CASE("ToolScheduler: a cancellation-ignoring tool is named as cancellation_lag without stalling the batch (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  // Cancel-aware: its sleep resolves to `cancelled` the moment the scheduler
  // emits, so it winds down well inside the 100 ms grace window.
  add_latency_tool(registry, "FakeCancelAware", 5s);
  // Cancellation-ignoring: runs its full 1 s regardless of the emitted cancel.
  add_uncancellable_tool(registry, "FakeStuck", 1s);
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "FakeCancelAware"));
  batch.push_back(call(1, "FakeStuck"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point cancel_at{};
  std::chrono::steady_clock::time_point result_at{};
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                     result_at = std::chrono::steady_clock::now();
                                   }));

  // Let the scheduler spawn its children and enter their handlers before the
  // parent cancellation lands.
  asio::post(io, [&] {
    asio::post(io, [&] {
      cancel_at = std::chrono::steady_clock::now();
      signal.emit(asio::cancellation_type::terminal);
    });
  });

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  // Drain the cancellation-ignoring tool so the stack-scoped registry / audit
  // sink outlive the detached child that is still winding down.
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // 100 ms guarantee: the batch returned without blocking on the 1 s laggard.
  // The 500 ms bound is a 5x margin over the grace window and well under the
  // laggard's own latency, so a regression that re-introduced the unbounded
  // drain would push this past 1 s and fail here.
  REQUIRE(result_at - cancel_at < 500ms);

  // Exactly one `cancellation_lag` row, naming the cancellation-ignoring tool;
  // the cancel-aware tool wound down inside the grace window and is not named.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 1);
  const auto stuck_named = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.tool_name == "FakeStuck";
  });
  REQUIRE(stuck_named);
  const auto cancelaware_named = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.tool_name == "FakeCancelAware";
  });
  REQUIRE_FALSE(cancelaware_named);
  // The row carries the `error_kind=cancellation_lag` marker spec 0012 AC5 names.
  const auto marked = std::ranges::any_of(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag" && e.metadata_json.contains("cancellation_lag");
  });
  REQUIRE(marked);
}

TEST_CASE("ToolScheduler: a detached cancellation laggard outlives scheduler-owned path locks safely",
          "[unit][agent][scheduler][cancellation][lock]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  TempDir root{"oran-sched-laggard-lifetime"};
  const auto rel = std::string{"held.txt"};
  touch_file(root.path() / rel);

  auto release_gate = std::make_shared<async::Channel<bool>>(io.get_executor(), 1);
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto finished = std::make_shared<std::atomic<bool>>(false);
  tool::Registry registry;
  add_gated_uncancellable_write_tool(registry, "FakeStuckWrite", release_gate, started, finished);
  auto workspace = make_workspace(root.path());
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype_with_workspace(io, rules, audit, workspace);

  std::optional<agent::ToolScheduler> scheduler;
  scheduler.emplace(io.get_executor(),
                    registry,
                    agent::ToolSchedulerOptions{.max_parallel_tools = 1, .per_call_timeout = 30s});
  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "FakeStuckWrite", write_call_input(rel)));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler->run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  const auto start_deadline = std::chrono::steady_clock::now() + 2s;
  while (!started->load(std::memory_order_acquire) && !failure && std::chrono::steady_clock::now() < start_deadline) {
    io.run_one();
  }
  REQUIRE(started->load(std::memory_order_acquire));

  signal.emit(asio::cancellation_type::terminal);
  const auto result_deadline = std::chrono::steady_clock::now() + 2s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < result_deadline) {
    io.run_one();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));
  REQUIRE_FALSE(finished->load(std::memory_order_acquire));

  // Destroy the public owner while the detached child still holds the path
  // lock, then let the child finish and release its guard. The child-held state
  // keeps the lock table alive until this release completes.
  scheduler.reset();
  REQUIRE(release_gate->try_send(true).has_value());
  io.restart();
  io.run();
  REQUIRE(finished->load(std::memory_order_acquire));
}

TEST_CASE("ToolScheduler: cancel-aware tools record no cancellation_lag rows on parent cancel (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "FakeSlow", 1s);
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  batch.push_back(call(0, "FakeSlow"));
  batch.push_back(call(1, "FakeSlow"));

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // Cancel-aware tools all wound down inside the grace window: no tool is a
  // cancellation laggard, so the audit log carries zero `cancellation_lag` rows.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 0);
}

TEST_CASE("ToolScheduler: a queued call cancelled before it runs is not mis-named as cancellation_lag (AC5)",
          "[unit][agent][scheduler][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  tool::Registry registry;
  add_latency_tool(registry, "FakeSlow", 1s);  // cancel-aware
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto prototype = make_prototype(io, rules, audit);

  // max_parallel_tools = 2 < batch size, so four of the six calls sit queued on
  // the admission queue when the parent cancel lands. A queued call never
  // ran a handler, so it must report its (cancelled) completion rather than be
  // mis-named as a cancellation laggard.
  agent::ToolScheduler scheduler{io.get_executor(),
                                 registry,
                                 agent::ToolSchedulerOptions{.max_parallel_tools = 2, .per_call_timeout = 30s}};

  std::vector<agent::ToolBatchCall> batch;
  for (std::size_t i = 0; i < 6; ++i) {
    batch.push_back(call(i, "FakeSlow"));
  }

  std::optional<core::Result<std::vector<agent::ToolBatchResult>>> result;
  std::exception_ptr failure;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::vector<agent::ToolBatchResult>>> {
        co_return co_await scheduler.run_batch(std::move(batch), prototype);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::vector<agent::ToolBatchResult>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                   }));

  asio::post(io, [&] { asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); }); });

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!result.has_value() && !failure && std::chrono::steady_clock::now() < deadline) {
    io.run_one();
  }
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(error_has_context(result->error(), "reason", "parent_cancelled"));

  // The two running tools are cancel-aware and the four queued ones never
  // started a handler, so none ignored a cancellation slot: zero rows named.
  const auto lag_rows = std::ranges::count_if(audit.events(), [](const permission::AuditEvent& e) {
    return e.event_kind == "cancellation_lag";
  });
  REQUIRE(lag_rows == 0);
}

TEST_CASE("ToolScheduler: timeout includes final-path admission and joins only the waiting context",
          "[integration][agent][scheduler][admission][cancellation]") {
  asio::io_context io;
  TempDir root{"oran-sched-admission-timeout"};
  auto workspace = make_workspace(root.path());
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto holder_context = make_prototype_with_workspace(io, rules, audit, workspace);
  auto waiter_context = make_prototype_with_workspace(io, rules, audit, workspace);
  auto release = std::make_shared<async::Channel<bool>>(io.get_executor(), 1);
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto finished = std::make_shared<std::atomic<bool>>(false);
  tool::Registry registry;
  add_gated_uncancellable_write_tool(registry, "HoldPath", release, started, finished);
  REQUIRE(tool::register_file_write(registry).has_value());

  std::vector<hook::ToolAfterPayload> observations;
  hook::Bus bus;
  hook::Sink sink{
      .id = "rewrite-waiter",
      .observe = [&observations](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<void> {
        if (event == hook::Event::tool_after) {
          observations.push_back(std::get<hook::ToolAfterPayload>(*payload));
        }
        co_return;
      },
      .decide = [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<hook::HookDecision>> {
        hook::HookDecision decision;
        decision.kind = hook::HookDecisionKind::rewrite;
        decision.rewritten_input_json = R"({"path":"held.txt","content":"recovered"})";
        co_return decision;
      },
      .trusted_local = true,
  };
  bus.subscribe(sink, {hook::Event::tool_before, hook::Event::tool_after});
  waiter_context.bus = &bus;

  agent::ToolScheduler scheduler{io.get_executor(), registry, {.max_parallel_tools = 1, .per_call_timeout = 40ms}};
  using BatchResult = core::Result<std::vector<agent::ToolBatchResult>>;
  std::optional<BatchResult> holder;
  std::optional<BatchResult> waiter;
  std::optional<BatchResult> recovered;
  std::exception_ptr failure;
  auto completion = [&failure](std::optional<BatchResult>& result) {
    return [&failure, &result](std::exception_ptr error, BatchResult value) {
      if (error)
        failure = error;
      result = std::move(value);
    };
  };
  auto poll = [&io] {
    io.restart();
    io.poll();
  };

  asio::co_spawn(io,
                 scheduler.run_batch({call(0, "HoldPath", write_call_input("held.txt"))}, holder_context),
                 completion(holder));
  poll();
  asio::co_spawn(
      io,
      scheduler.run_batch({call(1, "FileWrite", R"({"path":"original.txt","content":"unused"})")}, waiter_context),
      completion(waiter));
  io.restart();
  io.run_for(80ms);
  const bool timed_out_while_held = waiter.has_value() && !holder.has_value();
  const bool no_effect = !std::filesystem::exists(root.path() / "held.txt");
  const auto observed_while_held = observations.size();

  bool waiter_joined = false;
  asio::co_spawn(io,
                 scheduler.wait_idle(&waiter_context),
                 [&waiter_joined](std::exception_ptr error, core::Result<void> result) {
                   waiter_joined = !error && result.has_value();
                 });
  poll();
  const bool joined_before_holder = waiter_joined;
  REQUIRE(release->try_send(true).has_value());
  poll();
  asio::co_spawn(
      io,
      scheduler.run_batch({call(2, "FileWrite", R"({"path":"original.txt","content":"unused"})")}, waiter_context),
      completion(recovered));
  poll();

  REQUIRE_FALSE(failure);
  REQUIRE(started->load());
  REQUIRE(finished->load());
  REQUIRE(timed_out_while_held);
  REQUIRE(no_effect);
  REQUIRE(observed_while_held == 1);
  REQUIRE(joined_before_holder);
  REQUIRE(waiter->has_value());
  REQUIRE_FALSE(waiter->value()[0].output.has_value());
  REQUIRE(error_has_context(waiter->value()[0].output.error(), "reason", "timeout"));
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->has_value());
  REQUIRE(recovered->value()[0].output.has_value());
  REQUIRE(std::filesystem::exists(root.path() / "held.txt"));
  REQUIRE_FALSE(std::filesystem::exists(root.path() / "original.txt"));
  REQUIRE_FALSE(observations[0].succeeded);
  REQUIRE(observations[0].error_kind == "cancelled");
}

TEST_CASE("ToolScheduler: invalid bounds fail before dispatch", "[agent][scheduler]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto context = make_prototype(io, rules, audit);
    for (const auto options : {agent::ToolSchedulerOptions{.max_parallel_tools = 0},
                               agent::ToolSchedulerOptions{.per_call_timeout = 0ms},
                               agent::ToolSchedulerOptions{.per_call_timeout = -1ms}}) {
      agent::ToolScheduler scheduler{io.get_executor(), registry, options};
      auto batch = std::vector{call(0, "unused")};
      auto result = co_await scheduler.run_batch(std::move(batch), context);
      REQUIRE_FALSE(result);
      CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
      auto drained = co_await scheduler.wait_idle(&context);
      REQUIRE(drained);
    }
  });
}

TEST_CASE("ToolScheduler: exceptional dispatch completes promptly and queued calls progress", "[agent][scheduler]") {
  class ThrowOnceAudit final : public permission::AuditSink {
  public:
    async::Awaitable<core::Result<void>> record(permission::AuditEvent) override {
      if (calls++ == 0)
        throw std::runtime_error{"injected audit failure"};
      co_return core::Result<void>{};
    }
    int calls{0};
  };
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    add_latency_tool(registry, "Noop", 1ms);
    auto rules = allow_all_rules();
    ThrowOnceAudit audit;
    auto context = make_prototype(io, rules, audit);
    agent::ToolScheduler scheduler{io.get_executor(), registry, {.max_parallel_tools = 1, .per_call_timeout = 30s}};
    auto batch = std::vector{call(0, "Noop"), call(1, "Noop"), call(2, "Noop")};
    auto result = co_await scheduler.run_batch(std::move(batch), context);
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK_FALSE((*result)[0].output);
    CHECK((*result)[0].output.error().kind() == core::ErrorKind::internal);
    CHECK((*result)[1].output);
    CHECK((*result)[2].output);
    CHECK(audit.calls == 3);
    auto joined = co_await scheduler.wait_idle(&context);
    REQUIRE(joined);
  });
}
