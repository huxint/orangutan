#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::tool {
class Registry;
struct DispatchContext;
}  // namespace orangutan::tool

namespace orangutan::agent {

/// Per-batch concurrency and per-dispatch timeout bounds; both must be positive.
struct ToolSchedulerOptions {
  std::size_t max_parallel_tools{4};
  std::chrono::milliseconds per_call_timeout{60'000};

  friend bool operator==(const ToolSchedulerOptions&, const ToolSchedulerOptions&) = default;
};

/// A provider tool call; its ID is preserved in the corresponding result.
struct ToolBatchCall {
  std::string tool_use_id;
  std::string name;
  std::string input_json;
};

/// A result in original batch order, including dispatch or cancellation errors.
struct ToolBatchResult {
  std::string tool_use_id;
  std::string name;
  core::Result<tool::Output> output;
};

/// Bounded dispatch with shared path exclusion across batches and sessions.
/// All calls and context joins run on the supplied coordinating strand.
class ToolScheduler {
public:
  /// Borrow the registry until all dispatches finish. Each call receives its
  /// own context; the prototype's services remain borrowed until its join.
  ToolScheduler(asio::any_io_executor executor, tool::Registry& registry, ToolSchedulerOptions options = {});
  ~ToolScheduler();

  ToolScheduler(const ToolScheduler&) = delete;
  ToolScheduler& operator=(const ToolScheduler&) = delete;
  ToolScheduler(ToolScheduler&&) noexcept;
  ToolScheduler& operator=(ToolScheduler&&) noexcept;

  /// Admit at most max_parallel_tools coroutine frames; queued calls remain
  /// values until a slot completes. Invalid bounds fail before any effect.
  /// Return results in input order. Parent cancellation propagates to every
  /// call, then returns `parent_cancelled` after a 100 ms cleanup grace window
  /// and recording any lagging calls. Their path locks survive until cleanup.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<ToolBatchResult>>>
  run_batch(std::vector<ToolBatchCall> batch, tool::DispatchContext& prototype);

  /// Join dispatches that outlived a cancelled batch before releasing their
  /// borrowed contexts and services. A context selects only its own calls;
  /// null joins every call. Ignores cancellation while draining.
  [[nodiscard]] async::Awaitable<core::Result<void>> wait_idle(const tool::DispatchContext* context = nullptr);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent
