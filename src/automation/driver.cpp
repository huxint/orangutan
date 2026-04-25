#include "automation/driver.hpp"

#include "utils/scope-exit.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

#include <exec/timed_scheduler.hpp>
#include <stdexec/execution.hpp>

namespace orangutan::automation {

    Driver::Driver(Kernel &kernel, ExecutorPort &executor, utils::TaskPool &pool, std::string driver_id, ClockSource clock, std::size_t batch_limit)
    : kernel_(kernel),
      executor_(executor),
      pool_(pool),
      clock_(std::move(clock)),
      driver_id_(driver_id.empty() ? "automation-driver" : std::move(driver_id)),
      batch_limit_(batch_limit == 0 ? 1 : batch_limit) {}

    Driver::~Driver() {
        stop();
    }

    void Driver::start() {
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(mutex_);
            if (running_.load()) {
                return;
            }

            last_error_.reset();
            stop_source_ = std::stop_source{};
            scope_ = std::make_shared<exec::async_scope>();
            running_.store(true);
            cycle_active_.store(false);
            recovery_pending_.store(true);
            generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }

        spawn_cycle(std::chrono::steady_clock::duration::zero(), generation);
    }

    void Driver::stop() {
        std::shared_ptr<exec::async_scope> scope;
        {
            std::scoped_lock lock(mutex_);
            if (!running_.load() && scope_ == nullptr) {
                running_.store(false);
                return;
            }

            running_.store(false);
            stop_source_.request_stop();
            generation_.fetch_add(1, std::memory_order_acq_rel);
            scope = std::move(scope_);
        }

        if (scope != nullptr) {
            scope->request_stop();
            try {
                static_cast<void>(stdexec::sync_wait(scope->on_empty()));
            } catch (const std::exception &error) {
                record_error(error.what());
            } catch (...) {
                record_error("automation driver stopped with an unknown scope error");
            }
        }
        cycle_active_.store(false);
    }

    void Driver::wake() {
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(mutex_);
            if (!running_.load()) {
                return;
            }
            generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        spawn_cycle(std::chrono::steady_clock::duration::zero(), generation);
    }

    auto Driver::running() const noexcept -> bool {
        return running_.load();
    }

    auto Driver::last_error() const -> std::optional<std::string> {
        std::scoped_lock lock(mutex_);
        return last_error_;
    }

    auto Driver::current_time() const -> TimePoint {
        if (clock_ != nullptr) {
            return clock_();
        }
        return Clock::now();
    }

    void Driver::record_error(std::string error) {
        std::scoped_lock lock(mutex_);
        last_error_ = std::move(error);
    }

    void Driver::clear_last_error() {
        std::scoped_lock lock(mutex_);
        last_error_.reset();
    }

    void Driver::spawn_cycle(std::chrono::steady_clock::duration delay, std::uint64_t generation) {
        std::shared_ptr<exec::async_scope> scope;
        {
            std::scoped_lock lock(mutex_);
            if (!running_.load() || scope_ == nullptr || generation != generation_.load(std::memory_order_acquire)) {
                return;
            }
            scope = scope_;
        }

        auto task = exec::schedule_after(pool_.timed_scheduler(), delay) | stdexec::let_value([this, generation] {
                        return stdexec::schedule(pool_.scheduler()) | stdexec::then([this, generation] {
                                   run_cycle(generation);
                               });
                    });
        scope->spawn(std::move(task));
    }

    void Driver::run_cycle(std::uint64_t generation) {
        if (!running_.load() || stop_source_.stop_requested() || generation != generation_.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!cycle_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        std::optional<std::chrono::steady_clock::duration> next_delay;
        const auto release_cycle = utils::scope_exit([this, generation, &next_delay] {
            cycle_active_.store(false, std::memory_order_release);
            const auto current_generation = generation_.load(std::memory_order_acquire);
            if (running_.load() && current_generation != generation) {
                spawn_cycle(std::chrono::steady_clock::duration::zero(), current_generation);
                return;
            }
            if (running_.load() && next_delay.has_value()) {
                spawn_cycle(*next_delay, generation);
            }
        });

        const auto stop_token = stop_source_.get_token();
        if (stop_token.stop_requested()) {
            return;
        }

        const auto now = current_time();
        if (recovery_pending_.exchange(false)) {
            auto recovered = kernel_.recover(now, driver_id_);
            if (!recovered) {
                recovery_pending_.store(true);
                record_error(recovered.error());
                next_delay = std::chrono::seconds{1};
                return;
            }
        }

        auto next_wakeup = kernel_.next_wakeup(now);
        if (!next_wakeup) {
            record_error(next_wakeup.error());
            next_delay = std::chrono::seconds{1};
            return;
        }

        clear_last_error();

        if (!next_wakeup->has_value()) {
            return;
        }

        if (**next_wakeup > now) {
            auto delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(**next_wakeup - now);
            if (delay < std::chrono::steady_clock::duration::zero()) {
                delay = std::chrono::steady_clock::duration::zero();
            }
            next_delay = delay;
            return;
        }

        auto dispatches = kernel_.reserve_due(now, batch_limit_, driver_id_);
        if (!dispatches) {
            record_error(dispatches.error());
            next_delay = std::chrono::seconds{1};
            return;
        }
        if (dispatches->empty()) {
            return;
        }

        for (const auto &request : *dispatches) {
            auto executed = execute_request(request, stop_token);
            if (!executed) {
                record_error(executed.error());
                next_delay = std::chrono::seconds{1};
                return;
            }
            clear_last_error();

            if (stop_token.stop_requested()) {
                break;
            }
        }

        if (running_.load() && !stop_token.stop_requested() && generation == generation_.load(std::memory_order_acquire)) {
            next_delay = std::chrono::steady_clock::duration::zero();
        }
    }

    auto Driver::execute_request(const DispatchRequest &request, std::stop_token stop_token) -> KernelResult<void> {
        auto started = kernel_.mark_started(request.execution_id, current_time());
        if (!started) {
            return std::unexpected(started.error());
        }

        const ExecutionContext context{
            .job_id = request.job_id,
            .execution_id = request.execution_id,
            .scheduled_for = request.scheduled_for,
            .stop_token = stop_token,
        };

        auto final_result = execute_with_retries(request, context);

        auto finished = kernel_.mark_finished(request.execution_id, final_result, current_time());
        if (!finished) {
            return std::unexpected(finished.error());
        }

        return {};
    }

    auto Driver::execute_with_retries(const DispatchRequest &request, const ExecutionContext &context) -> ExecutionResult {
        const auto max_retries = std::max(0, request.execution.max_retry_attempts);
        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            if (context.stop_token.stop_requested()) {
                return failed_execution("automation dispatch stopped");
            }

            ExecutorResult dispatched;
            try {
                dispatched = executor_.dispatch(request, context);
            } catch (const std::exception &error) {
                dispatched = std::unexpected(std::string(error.what()));
            } catch (...) {
                dispatched = std::unexpected(std::string("executor threw unknown exception"));
            }

            auto result = dispatched.has_value() ? std::move(*dispatched) : failed_execution(dispatched.error());
            if (result.success || attempt == max_retries) {
                return result;
            }

            const auto delay = retry_delay(request.execution, attempt);
            if (!wait_retry_delay(delay, context.stop_token)) {
                return failed_execution("automation retry stopped");
            }
        }

        return failed_execution("automation dispatch failed");
    }

    auto Driver::retry_delay(const ExecutionPolicy &policy, int failed_attempt) -> std::chrono::milliseconds {
        if (policy.initial_backoff <= std::chrono::milliseconds{0}) {
            return std::chrono::milliseconds{0};
        }

        const auto shift = std::min(failed_attempt, std::numeric_limits<int>::digits - 2);
        auto delay = policy.initial_backoff * (std::int64_t{1} << shift);
        if (policy.max_backoff > std::chrono::milliseconds{0}) {
            delay = std::min(delay, policy.max_backoff);
        }
        return delay;
    }

    auto Driver::wait_retry_delay(std::chrono::milliseconds delay, std::stop_token stop_token) -> bool {
        if (delay <= std::chrono::milliseconds{0}) {
            return !stop_token.stop_requested();
        }

        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        static_cast<void>(cv.wait_for(lock, stop_token, delay, [] {
            return false;
        }));
        return !stop_token.stop_requested();
    }

} // namespace orangutan::automation
