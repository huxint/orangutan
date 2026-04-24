#pragma once

#include "automation/action-registry.hpp"
#include "automation/kernel.hpp"
#include "utils/task-pool.hpp"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

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
        void run_loop(std::stop_token stop_token);

        [[nodiscard]]
        auto execute_request(const DispatchRequest &request, std::stop_token stop_token) -> KernelResult<void>;

        [[nodiscard]]
        auto wait_until(std::optional<TimePoint> wake_time, std::stop_token stop_token) -> bool;

        Kernel &kernel_;
        ExecutorPort &executor_;
        utils::TaskPool &pool_;
        ClockSource clock_;
        std::string driver_id_;
        std::size_t batch_limit_ = 16;
        mutable std::mutex mutex_;
        std::condition_variable_any cv_;
        std::optional<std::string> last_error_;
        std::jthread thread_;
        std::atomic<bool> running_{false};
        bool wake_requested_ = false;
    };

} // namespace orangutan::automation
