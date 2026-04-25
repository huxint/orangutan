#pragma once

#include "automation/action-registry.hpp"
#include "automation/kernel.hpp"
#include "utils/task-pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include <exec/async_scope.hpp>

namespace orangutan::automation {

    class Driver {
    public:
        using ClockSource = std::function<TimePoint()>;

        Driver(Kernel &kernel, ExecutorPort &executor, utils::TaskPool &pool, std::string driver_id, ClockSource clock = {}, std::size_t batch_limit = 16);
        ~Driver();

        Driver(const Driver &) = delete;
        auto operator=(const Driver &) -> Driver & = delete;
        Driver(Driver &&) = delete;
        auto operator=(Driver &&) -> Driver & = delete;

        void start();
        void stop();
        void wake();

        [[nodiscard]]
        auto running() const noexcept -> bool;

        [[nodiscard]]
        auto last_error() const -> std::optional<std::string>;

    private:
        [[nodiscard]]
        auto current_time() const -> TimePoint;

        void record_error(std::string error);
        void clear_last_error();
        void spawn_cycle(std::chrono::steady_clock::duration delay, std::uint64_t generation);
        void run_cycle(std::uint64_t generation);

        [[nodiscard]]
        auto execute_request(const DispatchRequest &request, std::stop_token stop_token) -> KernelResult<void>;

        [[nodiscard]]
        auto execute_with_retries(const DispatchRequest &request, const ExecutionContext &context) -> ExecutionResult;

        [[nodiscard]]
        static auto retry_delay(const ExecutionPolicy &policy, int failed_attempt) -> std::chrono::milliseconds;

        [[nodiscard]]
        static auto wait_retry_delay(std::chrono::milliseconds delay, std::stop_token stop_token) -> bool;

        Kernel &kernel_;
        ExecutorPort &executor_;
        utils::TaskPool &pool_;
        ClockSource clock_;
        std::string driver_id_;
        std::size_t batch_limit_ = 16;
        mutable std::mutex mutex_;
        std::optional<std::string> last_error_;
        std::shared_ptr<exec::async_scope> scope_;
        std::stop_source stop_source_;
        std::atomic<bool> running_{false};
        std::atomic<bool> cycle_active_{false};
        std::atomic<bool> recovery_pending_{false};
        std::atomic<std::uint64_t> generation_{0};
    };

} // namespace orangutan::automation
