#pragma once

#include "automation/service.hpp"
#include "utils/transparent-lookup.hpp"
#include "utils/task-pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <exec/async_scope.hpp>

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

        AutomationRuntime(AutomationService &service, utils::TaskPool &pool, ClockSource clock = {});
        ~AutomationRuntime();

        AutomationRuntime(const AutomationRuntime &) = delete;
        AutomationRuntime &operator=(const AutomationRuntime &) = delete;
        AutomationRuntime(AutomationRuntime &&) = delete;
        AutomationRuntime &operator=(AutomationRuntime &&) = delete;

        void start();
        void stop();
        void set_executor(AutomationExecutor executor);
        void add_delivery_filter(AutomationDeliveryFilter filter);
        void register_category(AutomationCategory category);
        void set_notifier(AutomationNotifier notifier);
        void dispatch_background(std::function<void()> work);

        void run_pending(TimePoint now);

        [[nodiscard]]
        AgentExecutionLease acquire_agent_execution_lease(std::string_view agent_key);

        [[nodiscard]]
        AutomationService &service() noexcept;

        [[nodiscard]]
        const AutomationService &service() const noexcept;

    private:
        [[nodiscard]]
        TimePoint current_time() const;

        [[nodiscard]]
        std::shared_ptr<AgentExecutionGate> get_agent_execution_gate(std::string_view agent_key);

        void spawn_cycle(std::chrono::steady_clock::duration delay, std::uint64_t generation);
        void run_cycle(std::uint64_t generation);

        [[nodiscard]]
        std::optional<std::chrono::steady_clock::duration> next_cycle_delay(TimePoint now) const;

        AutomationService *service_ = nullptr;
        utils::TaskPool *pool_ = nullptr;
        ClockSource clock_;
        std::shared_ptr<exec::async_scope> scope_;
        std::shared_ptr<std::atomic<bool>> background_stop_requested_;
        std::atomic<bool> running_{false};
        std::atomic<bool> cycle_active_{false};
        std::atomic<std::uint64_t> generation_{0};
        std::mutex scope_mutex_;
        std::mutex agent_execution_gates_mutex_;
        utils::transparent_string_unordered_map<std::weak_ptr<AgentExecutionGate>> agent_execution_gates_;
    };

    template <typename Fn>
    decltype(auto) with_agent_execution_lease(AutomationRuntime *runtime, std::string_view agent_key, Fn &&fn) {
        using Result = std::invoke_result_t<Fn>;
        std::optional<AutomationRuntime::AgentExecutionLease> lease;
        if (runtime != nullptr) {
            lease.emplace(runtime->acquire_agent_execution_lease(agent_key));
        }

        if constexpr (std::is_void_v<Result>) {
            std::invoke(std::forward<Fn>(fn));
            return;
        } else {
            return std::invoke(std::forward<Fn>(fn));
        }
    }

} // namespace orangutan::automation
