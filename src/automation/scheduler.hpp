#pragma once

#include "types/types.hpp"
#include "automation/log-writer.hpp"
#include "automation/automation-store.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orangutan::automation {

    class Runtime {
        class AgentExecutionGate;

    public:
        using Executor = std::function<ExecutionResult(const Trigger &)>;
        using Notifier = std::function<std::optional<std::string>(std::string_view target, std::string_view message)>;

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
            std::shared_ptr<AgentExecutionGate> gate_;
            std::thread::id owner_;

            void release() noexcept;
        };

        explicit Runtime(Store &store);
        ~Runtime();

        Runtime(const Runtime &) = delete;
        Runtime &operator=(const Runtime &) = delete;
        Runtime(Runtime &&) = delete;
        Runtime &operator=(Runtime &&) = delete;

        void set_executor(Executor executor);
        void set_notifier(Notifier notifier);

        void start();
        void stop();
        void run_pending(TimePoint now);
        [[nodiscard]]
        AgentExecutionLease acquire_agent_execution_lease(const std::string &agent_key);

        [[nodiscard]]
        std::vector<TaskSpec> list_tasks(const std::string &agent_key) const;
        [[nodiscard]]
        std::optional<TaskSpec> find_task(const std::string &agent_key, const std::string &id_or_name) const;
        [[nodiscard]]
        std::string save_task(const TaskSpec &task);
        [[nodiscard]]
        bool remove_task(const std::string &agent_key, const std::string &id_or_name);
        [[nodiscard]]
        std::string run_task_now(const std::string &agent_key, const std::string &id_or_name);

        [[nodiscard]]
        std::vector<HeartbeatSpec> list_heartbeats(const std::string &agent_key) const;
        [[nodiscard]]
        std::optional<HeartbeatSpec> find_heartbeat(const std::string &agent_key, const std::string &id_or_name) const;
        [[nodiscard]]
        std::string save_heartbeat(const HeartbeatSpec &heartbeat);
        [[nodiscard]]
        bool remove_heartbeat(const std::string &agent_key, const std::string &id_or_name);
        [[nodiscard]]
        bool pause_heartbeat(const std::string &agent_key, const std::string &id_or_name, bool paused);
        [[nodiscard]]
        std::string run_heartbeat_now(const std::string &agent_key, const std::string &id_or_name);

        [[nodiscard]]
        std::vector<InboxItem> list_inbox(const std::string &agent_key) const;
        [[nodiscard]]
        bool ack_inbox(const std::string &agent_key, const std::string &id);
        void clear_inbox(const std::string &agent_key);

        [[nodiscard]]
        Store &store() noexcept;
        [[nodiscard]]
        const Store &store() const noexcept;

    private:
        Store &store_;
        LogWriter log_writer_;
        Executor executor_;
        Notifier notifier_;
        std::thread worker_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> running_{false};
        base::i64 startup_time_ = 0;
        std::mutex agent_execution_gates_mutex_;
        std::unordered_map<std::string, std::weak_ptr<AgentExecutionGate>> agent_execution_gates_;

        void scheduler_loop();
        void normalize_state(TimePoint now);
        struct CompletedExecution {
            ExecutionResult result;
            base::i64 finished_at = 0;
            std::string status;
            std::string delivery_status = "silent";
            std::string log_path;
        };

        [[nodiscard]]
        std::shared_ptr<AgentExecutionGate> get_agent_execution_gate(const std::string &agent_key);
        [[nodiscard]]
        CompletedExecution execute_trigger(const Trigger &trigger, base::i64 started_at);
        void execute_task(const TaskSpec &task, std::optional<base::i64> forced_timestamp = std::nullopt);
        void execute_heartbeat(const HeartbeatSpec &heartbeat, std::optional<base::i64> forced_timestamp = std::nullopt);
        void record_delivery_failure(const Trigger &trigger, std::string_view run_id, std::string_view title, std::string_view body);
    };

    template <typename Fn>
    decltype(auto) with_agent_execution_lease(Runtime *runtime, std::string_view agent_key, Fn &&fn) {
        using Result = std::invoke_result_t<Fn>;
        std::optional<Runtime::AgentExecutionLease> lease;
        if (runtime != nullptr) {
            lease.emplace(runtime->acquire_agent_execution_lease(std::string(agent_key)));
        }

        if constexpr (std::is_void_v<Result>) {
            std::invoke(std::forward<Fn>(fn));
            return;
        } else {
            return std::invoke(std::forward<Fn>(fn));
        }
    }

} // namespace orangutan::automation
