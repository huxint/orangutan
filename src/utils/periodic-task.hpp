#pragma once

#include "utils/task-pool.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>

#include <exec/async_scope.hpp>
#include <exec/repeat_effect_until.hpp>
#include <exec/timed_scheduler.hpp>
#include <stdexec/execution.hpp>

namespace orangutan::utils {

    /// Cooperatively cancellable periodic task backed by TaskPool.
    ///
    /// Schedules `work` on the pool scheduler every `period`, using the
    /// shared timer thread for delays. `work` returns `true` to keep
    /// ticking or `false` to exit the loop (e.g. a heartbeat that hit a
    /// fatal send error). Exceptions from `work` are caught and the next
    /// tick retries.
    ///
    /// start()/stop() must be called from a single thread. Re-calling
    /// start() replaces the previous loop; stop() is idempotent.
    class PeriodicTask {
    public:
        using Work = std::function<bool()>;

        PeriodicTask() = default;
        ~PeriodicTask() { stop(); }

        PeriodicTask(const PeriodicTask &) = delete;
        PeriodicTask &operator=(const PeriodicTask &) = delete;
        PeriodicTask(PeriodicTask &&) = delete;
        PeriodicTask &operator=(PeriodicTask &&) = delete;

        void start(TaskPool &pool, std::chrono::steady_clock::duration period, Work work) {
            stop();
            scope_ = std::make_unique<exec::async_scope>();
            running_.store(true);

            auto tick = stdexec::schedule(pool.scheduler())
                      | stdexec::then([this, fn = std::move(work)]() mutable -> bool {
                            if (!running_.load()) {
                                return false;
                            }
                            try {
                                return fn();
                            } catch (...) { // NOLINT(bugprone-empty-catch): next tick retries
                                return true;
                            }
                        })
                      | stdexec::let_value([pool_ptr = &pool, period, this](bool should_continue) {
                            const auto delay = should_continue
                                ? period
                                : std::chrono::steady_clock::duration::zero();
                            return exec::schedule_after(pool_ptr->timed_scheduler(), delay)
                                 | stdexec::then([this, should_continue] {
                                       return !should_continue || !running_.load();
                                   });
                        })
                      | exec::repeat_effect_until();

            scope_->spawn(std::move(tick));
        }

        void stop() noexcept {
            if (scope_ == nullptr) {
                return;
            }
            running_.store(false);
            scope_->request_stop();
            try {
                static_cast<void>(stdexec::sync_wait(scope_->on_empty()));
            } catch (...) { // NOLINT(bugprone-empty-catch): scope teardown best-effort
            }
            scope_.reset();
        }

        [[nodiscard]]
        bool running() const noexcept {
            return running_.load();
        }

    private:
        std::atomic<bool> running_{false};
        std::unique_ptr<exec::async_scope> scope_;
    };

} // namespace orangutan::utils
