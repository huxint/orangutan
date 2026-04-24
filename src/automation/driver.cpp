#include "automation/driver.hpp"

#include <exception>
#include <tuple>
#include <utility>

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
        std::scoped_lock lock(mutex_);
        if (thread_.joinable()) {
            return;
        }

        last_error_.reset();
        wake_requested_ = false;
        running_.store(true);
        thread_ = std::jthread([this](std::stop_token stop_token) {
            run_loop(stop_token);
        });
    }

    void Driver::stop() {
        std::jthread thread;
        {
            std::scoped_lock lock(mutex_);
            if (!thread_.joinable()) {
                running_.store(false);
                return;
            }

            running_.store(false);
            wake_requested_ = true;
            thread = std::move(thread_);
        }

        thread.request_stop();
        cv_.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void Driver::wake() {
        {
            std::scoped_lock lock(mutex_);
            wake_requested_ = true;
        }
        cv_.notify_all();
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

    void Driver::run_loop(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            const auto now = current_time();
            auto next_wakeup = kernel_.next_wakeup(now);
            if (!next_wakeup) {
                record_error(next_wakeup.error());
                if (!wait_until(std::nullopt, stop_token)) {
                    break;
                }
                continue;
            }

            clear_last_error();

            if (!next_wakeup->has_value()) {
                if (!wait_until(std::nullopt, stop_token)) {
                    break;
                }
                continue;
            }

            if (**next_wakeup > now) {
                if (!wait_until(*next_wakeup, stop_token)) {
                    break;
                }
                continue;
            }

            auto dispatches = kernel_.reserve_due(now, batch_limit_, driver_id_);
            if (!dispatches) {
                record_error(dispatches.error());
                if (!wait_until(std::nullopt, stop_token)) {
                    break;
                }
                continue;
            }

            bool cycle_failed = false;
            for (const auto &request : *dispatches) {
                auto executed = execute_request(request, stop_token);
                if (!executed) {
                    record_error(executed.error());
                    cycle_failed = true;
                    break;
                }
                clear_last_error();

                if (stop_token.stop_requested()) {
                    break;
                }
            }

            if (cycle_failed) {
                if (!wait_until(std::nullopt, stop_token)) {
                    break;
                }
            }
        }

        running_.store(false);
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

        auto sender = stdexec::schedule(pool_.scheduler()) | stdexec::then([this, request, context]() -> ExecutorResult {
                          try {
                              return executor_.dispatch(request, context);
                          } catch (const std::exception &error) {
                              return std::unexpected(std::string(error.what()));
                          } catch (...) {
                              return std::unexpected(std::string("executor threw unknown exception"));
                          }
                      });

        std::optional<std::tuple<ExecutorResult>> execution;
        try {
            execution = stdexec::sync_wait(std::move(sender));
        } catch (const std::exception &error) {
            return std::unexpected(error.what());
        }

        if (!execution.has_value()) {
            return std::unexpected("driver dispatch stopped before completion");
        }

        auto [result] = std::move(*execution);
        auto final_result = result.has_value()
            ? std::move(*result)
            : ExecutionResult{
                  .success = false,
                  .summary = result.error(),
              };

        auto finished = kernel_.mark_finished(request.execution_id, final_result, current_time());
        if (!finished) {
            return std::unexpected(finished.error());
        }

        return {};
    }

    auto Driver::wait_until(std::optional<TimePoint> wake_time, std::stop_token stop_token) -> bool {
        std::unique_lock lock(mutex_);
        if (wake_requested_) {
            wake_requested_ = false;
            return !stop_token.stop_requested();
        }

        if (wake_time.has_value()) {
            static_cast<void>(cv_.wait_until(lock, stop_token, *wake_time, [this] {
                return wake_requested_;
            }));
        } else {
            static_cast<void>(cv_.wait(lock, stop_token, [this] {
                return wake_requested_;
            }));
        }

        wake_requested_ = false;
        return !stop_token.stop_requested();
    }

} // namespace orangutan::automation
