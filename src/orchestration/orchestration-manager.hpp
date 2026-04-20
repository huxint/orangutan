#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include "orchestration/agent-definition-registry.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/types.hpp"
#include "orchestration/team-manager.hpp"
#include "orchestration/worker-runtime.hpp"

namespace orangutan::memory {
    class MemoryStore;
}
namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::orchestration {

    class OrchestrationManagerBuilder;

    /// Environment for spawned agent execution.
    struct AgentExecutionEnvironment {
        const AgentDefinitionRegistry *definition_registry = nullptr;
        storage::SessionStore *session_store = nullptr;
        memory::MemoryStore *memory_store = nullptr;
        AgentMailbox *mailbox = nullptr;
        TeamManager *team_manager = nullptr;
    };

    /// Unified manager for one-shot worker and persistent teammate orchestration.
    ///
    /// Key design:
    /// - **worker** role: spawn → run → notify → done
    /// - **teammate** role: spawn → run → idle → wait_for_next_prompt → run → ...
    /// - Both share the same spawn/stop/notification infrastructure
    /// - Mailbox and team management are first-class concerns, not bolted on
    class OrchestrationManager {
    public:
        explicit OrchestrationManager(int max_concurrent_agents = 4);
        ~OrchestrationManager();

        OrchestrationManager(const OrchestrationManager &) = delete;
        OrchestrationManager &operator=(const OrchestrationManager &) = delete;
        OrchestrationManager(OrchestrationManager &&) = delete;
        OrchestrationManager &operator=(OrchestrationManager &&) = delete;

        void set_environment(AgentExecutionEnvironment env);
        void set_notification_callback(TaskNotificationCallback callback);
        void register_runtime_notification_handler(std::string runtime_key, RuntimeNotificationHandler handler);
        void unregister_runtime_notification_handler(const std::string &runtime_key);
        void set_worker_runtime_factory(WorkerRuntimeFactory factory);

        /// Create a builder for fluent OrchestrationManager configuration.
        [[nodiscard]]
        static OrchestrationManagerBuilder configure(OrchestrationManager &manager);

        /// Spawn a new agent. The role in the request determines lifecycle.
        [[nodiscard]]
        AgentSpawnResult spawn(const AgentSpawnRequest &request);

        /// Send a message to a running agent (by run_id or by agent name within a team).
        [[nodiscard]]
        auto send_message(const std::string &run_id, const std::string &from, const std::string &text) -> std::optional<std::string>;

        /// Send a message to an agent by name within the caller's team.
        [[nodiscard]]
        auto send_message_by_name(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text) -> std::optional<std::string>;

        /// Broadcast a message to all team members except the sender.
        auto broadcast_message(const std::string &team_id, const std::string &from, const std::string &text) -> std::optional<std::string>;

        /// Stop a running agent. Works for both workers and teammates.
        void stop(const std::string &run_id);

        /// Get the record for a specific run.
        [[nodiscard]]
        auto get_run(const std::string &run_id) const -> std::optional<AgentRunRecord>;

        /// List all currently active (queued, running, or idle) runs.
        [[nodiscard]]
        auto list_active_runs() const -> std::vector<AgentRunRecord>;

        /// Gracefully shut down all active runs.
        void shutdown();

    private:
        struct ActiveRun {
            AgentSpawnRequest request;
            AgentRunRecord record;
            std::stop_source stop_source;
            mutable std::mutex mutex;
            std::condition_variable_any cv;
            bool completed = false;
        };

        struct Impl;

        int max_concurrent_;
        AgentExecutionEnvironment env_;
        TaskNotificationCallback notification_callback_;
        std::unordered_map<std::string, RuntimeNotificationHandler> runtime_notification_handlers_;
        WorkerRuntimeFactory worker_runtime_factory_;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<ActiveRun>> active_runs_;
        std::deque<std::string> pending_run_ids_;
        bool shutting_down_ = false;
        std::uint64_t next_run_id_ = 0;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]]
        auto make_run_id() -> std::string;

        [[nodiscard]]
        auto count_running_locked() const -> int;

        void launch_run_locked(const std::shared_ptr<ActiveRun> &run);
        void remove_pending_run_locked(const std::string &run_id);
        void maybe_start_queued_runs();

        /// Core worker execution loop. Handles both worker and teammate lifecycles.
        void run_worker(const std::shared_ptr<ActiveRun> &run, const std::stop_token &stop_token);

        /// Teammate-specific: idle loop waiting for follow-up prompts.
        void run_teammate_idle_loop(const std::shared_ptr<ActiveRun> &run, const std::unique_ptr<WorkerRuntime> &worker, const std::stop_token &stop_token);

        /// Deliver task notification to the parent runtime.
        void deliver_notification(const std::shared_ptr<ActiveRun> &run);
    };

    class OrchestrationManagerBuilder {
    public:
        explicit OrchestrationManagerBuilder(OrchestrationManager &manager) : manager_(manager) {}

        auto with_environment(this auto &&self, AgentExecutionEnvironment env) -> decltype(auto) {
            self.manager_.set_environment(env);
            return std::forward<decltype(self)>(self);
        }

        auto with_notification_callback(this auto &&self, TaskNotificationCallback callback) -> decltype(auto) {
            self.manager_.set_notification_callback(std::move(callback));
            return std::forward<decltype(self)>(self);
        }

        auto with_worker_runtime_factory(this auto &&self, WorkerRuntimeFactory factory) -> decltype(auto) {
            self.manager_.set_worker_runtime_factory(std::move(factory));
            return std::forward<decltype(self)>(self);
        }

        /// Terminal: returns a reference to the configured manager.
        [[nodiscard]]
        OrchestrationManager &build() const { return manager_; }

    private:
        OrchestrationManager &manager_;
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::AgentExecutionEnvironment;
    using orchestration::OrchestrationManager;
} // namespace orangutan
