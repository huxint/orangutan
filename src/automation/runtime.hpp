#pragma once

#include "automation/service.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace orangutan::automation {

    /// Coordinates background scheduling and per-agent execution serialization.
    class AutomationRuntime {
        class AgentExecutionGate;

    public:
        using ClockSource = std::function<TimePoint()>;

        class AgentExecutionLease {
        public:
            AgentExecutionLease() = default;
            explicit AgentExecutionLease(std::shared_ptr<AgentExecutionGate> gate);

            AgentExecutionLease(const AgentExecutionLease &) = delete;
            AgentExecutionLease &operator=(const AgentExecutionLease &) = delete;
            AgentExecutionLease(AgentExecutionLease &&other) noexcept;
            AgentExecutionLease &operator=(AgentExecutionLease &&other) noexcept;
            ~AgentExecutionLease();

        private:
            void release() noexcept;

            std::shared_ptr<AgentExecutionGate> gate_;
            std::thread::id owner_;
        };

        explicit AutomationRuntime(AutomationService &service, ClockSource clock = {});
        ~AutomationRuntime();

        AutomationRuntime(const AutomationRuntime &) = delete;
        AutomationRuntime &operator=(const AutomationRuntime &) = delete;
        AutomationRuntime(AutomationRuntime &&) = delete;
        AutomationRuntime &operator=(AutomationRuntime &&) = delete;

        void start();
        void stop();
        void run_pending(TimePoint now);

        [[nodiscard]]
        AgentExecutionLease acquire_agent_execution_lease(std::string_view agent_key);

    private:
        void scheduler_loop(std::stop_token stop_token);

        [[nodiscard]]
        TimePoint current_time() const;

        [[nodiscard]]
        std::shared_ptr<AgentExecutionGate> get_agent_execution_gate(std::string_view agent_key);

        AutomationService *service_ = nullptr;
        ClockSource clock_;
        std::jthread worker_;
        std::atomic<bool> running_{false};
        mutable std::mutex mutex_;
        std::condition_variable_any cv_;
        std::mutex agent_execution_gates_mutex_;
        std::unordered_map<std::string, std::weak_ptr<AgentExecutionGate>> agent_execution_gates_;
    };

} // namespace orangutan::automation
