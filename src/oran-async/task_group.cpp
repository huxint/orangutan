// src/oran-async/task_group.cpp — bounded ownership for named child tasks.

#include <oran/async/task_group.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <expected>
#include <mutex>
#include <ranges>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/channel.hpp>
#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>

namespace orangutan::async {

namespace {

struct ChildRecord {
  std::uint64_t sequence{};
  std::string name;
  std::shared_ptr<asio::cancellation_signal> cancellation;
};

struct CompletedRecord {
  std::uint64_t sequence{};
  TaskOutcome outcome;
};

[[nodiscard]] TaskOutcome failed_outcome(std::string name, core::Error error) {
  const auto status =
      error.kind() == core::ErrorKind::cancelled ? TaskOutcomeStatus::cancelled : TaskOutcomeStatus::failed;
  return TaskOutcome{.name = std::move(name), .status = status, .error = std::move(error)};
}

/// Cancel-aware sleep to an absolute deadline. Races against a completion
/// receive inside `join_with_timeout`; a parent cancellation resolves the
/// race on either operand, which the join handles identically.
[[nodiscard]] async::Awaitable<core::Result<void>> sleep_until(std::chrono::steady_clock::time_point deadline) {
  const auto executor = co_await asio::this_coro::executor;
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = deadline > now ? deadline - now : std::chrono::steady_clock::duration::zero();
  co_return co_await async::sleep_for(executor, std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
}

}  // namespace

struct TaskGroup::Impl : std::enable_shared_from_this<TaskGroup::Impl> {
  Impl(asio::any_io_executor exec, TaskGroupOptions opts)
      : executor{asio::make_strand(std::move(exec))}, options{opts}, completion{executor, 1} {}

  asio::any_io_executor executor;
  TaskGroupOptions options;
  Channel<std::monostate> completion;
  mutable std::mutex mutex;
  std::vector<std::shared_ptr<ChildRecord>> active;
  std::deque<CompletedRecord> completed;
  std::uint64_t next_sequence{};
  std::size_t completed_dropped{};
  bool closed{false};
  bool stop_requested{false};
  bool join_started{false};

  [[nodiscard]] core::Result<void> spawn(std::string name, Task task) {
    if (name.empty()) {
      return std::unexpected(core::Error::invalid_argument("task group child name must be non-empty"));
    }
    if (!task) {
      return std::unexpected(core::Error::invalid_argument("task group child task is empty"));
    }

    auto child = std::make_shared<ChildRecord>();
    child->name = std::move(name);
    child->cancellation = std::make_shared<asio::cancellation_signal>();
    {
      const std::scoped_lock lock{mutex};
      if (closed) {
        return std::unexpected(core::Error{core::ErrorKind::conflict, "task group is closed"});
      }
      if (active.size() >= options.max_tasks) {
        return std::unexpected(core::Error{core::ErrorKind::mailbox_overflowed, "task group capacity reached"}.with(
            "max_tasks",
            std::to_string(options.max_tasks)));
      }
      child->sequence = next_sequence++;
      active.push_back(child);
    }

    auto self = shared_from_this();
    try {
      // asio keeps using the bound cancellation signal *after* the child's own
      // coroutine frame is destroyed: `co_spawn`'s entry point emplaces this
      // thread of execution's `cancellation_state` into the bound slot, and it
      // still resets and clears that state once `co_await function()` returns.
      // The child frame must therefore never be the signal's last owner —
      // `finish()` drops the group's reference before the frame unwinds, so the
      // completion handler holds the record (and the group state) until asio is
      // done with both. Owning the record only from the frame frees the
      // cancellation state under asio's feet, and the recycled block then
      // corrupts whichever live coroutine's cancellation state reuses it.
      asio::co_spawn(
          executor,
          run_child(self, child, std::move(task)),
          asio::bind_cancellation_slot(
              child->cancellation->slot(),
              [state = self, record = child](std::exception_ptr error) {
                if (!error) {
                  return;
                }
                // `run_child` reports its own outcome and swallows
                // child exceptions; reaching here means the frame
                // itself failed, so the record would otherwise stay
                // active forever and stall `join()`.
                state->finish(record,
                              failed_outcome(record->name, core::Error::internal("task group child frame terminated")));
              }));
    } catch (const std::exception& error) {
      finish(child,
             failed_outcome(child->name,
                            core::Error::internal("task group failed to spawn child").with("reason", error.what())));
    } catch (...) {
      finish(child,
             failed_outcome(child->name,
                            core::Error::internal("task group failed to spawn child").with("reason", "unknown")));
    }
    return {};
  }

  void close() noexcept {
    const std::scoped_lock lock{mutex};
    closed = true;
  }

  void request_stop() noexcept {
    auto signals = std::vector<std::shared_ptr<asio::cancellation_signal>>{};
    {
      const std::scoped_lock lock{mutex};
      closed = true;
      if (stop_requested) {
        return;
      }
      stop_requested = true;
      signals.reserve(active.size());
      for (const auto& child : active) {
        signals.push_back(child->cancellation);
      }
    }

    for (auto& signal : signals) {
      asio::post(executor, [signal = std::move(signal)] { signal->emit(asio::cancellation_type::all); });
    }
  }

  [[nodiscard]] std::size_t active_tasks() const noexcept {
    const std::scoped_lock lock{mutex};
    return active.size();
  }

  [[nodiscard]] core::Result<void> begin_join() {
    const std::scoped_lock lock{mutex};
    closed = true;
    if (join_started) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "task group join already started"});
    }
    join_started = true;
    return {};
  }

  [[nodiscard]] TaskGroupReport drain_completed() {
    const std::scoped_lock lock{mutex};
    auto report = TaskGroupReport{};
    report.tasks.reserve(completed.size());
    std::ranges::sort(completed, {}, &CompletedRecord::sequence);
    for (auto& child : completed) {
      report.tasks.push_back(std::move(child.outcome));
    }
    completed.clear();
    report.outcomes_dropped = std::exchange(completed_dropped, 0);
    return report;
  }

  /// Full spawn-ordered report after a bounded join: completed outcomes merged
  /// with `lagging` rows for children still active at the deadline. Drains the
  /// completed queue like `drain_completed`; active children are NOT removed —
  /// an abandoned child records its outcome later through `finish`, which is
  /// safe because every spawned child retains the group state.
  [[nodiscard]] TaskGroupReport timed_join_report() {
    const std::scoped_lock lock{mutex};
    auto report = TaskGroupReport{};
    report.tasks.reserve(completed.size() + active.size());
    std::ranges::sort(completed, {}, &CompletedRecord::sequence);
    auto active_sorted = std::vector<std::shared_ptr<ChildRecord>>{active.begin(), active.end()};
    std::ranges::sort(active_sorted, {}, &ChildRecord::sequence);
    auto completed_it = completed.begin();
    auto active_it = active_sorted.begin();
    while (completed_it != completed.end() || active_it != active_sorted.end()) {
      if (completed_it != completed.end() &&
          (active_it == active_sorted.end() || completed_it->sequence < (*active_it)->sequence)) {
        report.tasks.push_back(std::move(completed_it->outcome));
        ++completed_it;
      } else {
        report.tasks.push_back(TaskOutcome{.name = (*active_it)->name, .status = TaskOutcomeStatus::lagging});
        ++active_it;
      }
    }
    completed.clear();
    report.outcomes_dropped = std::exchange(completed_dropped, 0);
    return report;
  }

private:
  void finish(const std::shared_ptr<ChildRecord>& child, TaskOutcome outcome) {
    {
      const std::scoped_lock lock{mutex};
      const auto found = std::ranges::find(active, child);
      if (found == active.end()) {
        return;
      }
      active.erase(found);
      if (completed.size() == options.max_completed) {
        completed.pop_front();
        ++completed_dropped;
      }
      completed.push_back(CompletedRecord{.sequence = child->sequence, .outcome = std::move(outcome)});
    }
    static_cast<void>(completion.try_send(std::monostate{}));
  }

  [[nodiscard]] static Awaitable<void>
  run_child(std::shared_ptr<Impl> state, std::shared_ptr<ChildRecord> child, Task task) {
    auto outcome = TaskOutcome{.name = child->name};
    try {
      auto result = co_await task();
      if (!result) {
        outcome = failed_outcome(std::move(outcome.name), std::move(result).error());
      }
    } catch (const asio::system_error& error) {
      if (error.code() == asio::error::operation_aborted) {
        outcome = failed_outcome(std::move(outcome.name), core::Error::cancelled());
      } else {
        outcome = failed_outcome(std::move(outcome.name),
                                 core::Error::internal("task group child threw asio error")
                                     .with("reason", error.what())
                                     .with("asio_error", error.code().message()));
      }
    } catch (const std::exception& error) {
      outcome = failed_outcome(std::move(outcome.name),
                               core::Error::internal("task group child threw").with("reason", error.what()));
    } catch (...) {
      outcome = failed_outcome(std::move(outcome.name),
                               core::Error::internal("task group child threw").with("reason", "unknown"));
    }

    state->finish(child, std::move(outcome));
  }
};

std::size_t TaskGroupReport::succeeded() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::succeeded, &TaskOutcome::status));
}

std::size_t TaskGroupReport::failed() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::failed, &TaskOutcome::status));
}

std::size_t TaskGroupReport::cancelled() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::cancelled, &TaskOutcome::status));
}

std::size_t TaskGroupReport::lagging() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(tasks, TaskOutcomeStatus::lagging, &TaskOutcome::status));
}

bool TaskGroupReport::all_succeeded() const noexcept {
  return outcomes_dropped == 0 && failed() == 0 && cancelled() == 0 && lagging() == 0;
}

core::Result<TaskGroup> TaskGroup::create(asio::any_io_executor executor, TaskGroupOptions options) {
  if (options.max_tasks == 0) {
    return std::unexpected(core::Error::invalid_argument("task group max_tasks must be positive"));
  }
  if (options.max_completed == 0) {
    return std::unexpected(core::Error::invalid_argument("task group max_completed must be positive"));
  }
  return TaskGroup{std::make_shared<Impl>(std::move(executor), options)};
}

TaskGroup::TaskGroup(std::shared_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

TaskGroup::~TaskGroup() {
  if (impl_) {
    impl_->request_stop();
  }
}

TaskGroup::TaskGroup(TaskGroup&&) noexcept = default;

TaskGroup& TaskGroup::operator=(TaskGroup&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      impl_->request_stop();
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

core::Result<void> TaskGroup::spawn(std::string name, Task task) {
  if (!impl_) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "task group has been moved from"});
  }
  return impl_->spawn(std::move(name), std::move(task));
}

TaskGroupReport TaskGroup::drain_completed() {
  return impl_ ? impl_->drain_completed() : TaskGroupReport{};
}

void TaskGroup::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

void TaskGroup::request_stop() noexcept {
  if (impl_) {
    impl_->request_stop();
  }
}

Awaitable<core::Result<TaskGroupReport>> TaskGroup::join() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!impl_) {
    co_return std::unexpected(core::Error{core::ErrorKind::conflict, "task group has been moved from"});
  }
  if (auto begun = impl_->begin_join(); !begun) {
    co_return std::unexpected(std::move(begun).error());
  }

  while (impl_->active_tasks() != 0) {
    auto completed = co_await impl_->completion.receive();
    if (!completed) {
      impl_->request_stop();
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    }
  }
  co_return impl_->drain_completed();
}

Awaitable<core::Result<TaskGroupReport>> TaskGroup::join_with_timeout(std::chrono::milliseconds timeout) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!impl_) {
    co_return std::unexpected(core::Error{core::ErrorKind::conflict, "task group has been moved from"});
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    co_return std::unexpected(core::Error::invalid_argument("task group join timeout must be positive"));
  }
  if (auto begun = impl_->begin_join(); !begun) {
    co_return std::unexpected(std::move(begun).error());
  }

  using namespace asio::experimental::awaitable_operators;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (impl_->active_tasks() != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    // One total deadline bounds the whole join, including the parent-cancelled
    // path below. Each iteration re-races a fresh remaining-time sleep so the
    // deadline stays absolute across wakeups.
    auto raced = co_await (impl_->completion.receive() || sleep_until(deadline));
    auto* completed = std::get_if<core::Result<std::monostate>>(&raced);
    if (completed != nullptr) {
      if (!*completed) {
        // Parent cancelled: request stop and keep waiting for cancel-aware
        // children to wind down — still bounded by the deadline.
        impl_->request_stop();
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
      }
      continue;
    }
    auto timer_result = std::get<core::Result<void>>(std::move(raced));
    if (!timer_result) {
      // Parent cancelled while parked on the deadline sleep; same as a
      // cancelled receive — keep waiting until the deadline.
      impl_->request_stop();
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
      continue;
    }
    break;
  }

  if (impl_->active_tasks() != 0) {
    // Deadline expired with children still running. Emit cancellation so
    // cancel-aware children wind down, then abandon the rest: the report
    // marks them `lagging` and the caller resumes owning their lifetime.
    impl_->request_stop();
    co_return impl_->timed_join_report();
  }
  co_return impl_->drain_completed();
}

std::size_t TaskGroup::active_tasks() const noexcept {
  return impl_ ? impl_->active_tasks() : 0;
}

TaskGroupOptions TaskGroup::options() const noexcept {
  return impl_ ? impl_->options : TaskGroupOptions{};
}

}  // namespace orangutan::async
