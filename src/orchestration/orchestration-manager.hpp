#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "orchestration/types.hpp"
#include "orchestration/teammate-runtime.hpp"

namespace orangutan::memory {
    class MemoryStore;
}
namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::orchestration {

    /// Environment for spawned agent execution.
    struct AgentExecutionEnvironment {
        storage::SessionStore *session_store = nullptr;
        memory::MemoryStore *memory_store = nullptr;
        AgentMailbox *mailbox = nullptr;
        TeamManager *team_manager = nullptr;
    };

    /// Unified manager for spawned teammate orchestration.
    ///
    /// Teammates share scheduling, stop tokens, notifications, mailbox delivery,
    /// and team membership. Each teammate runs an initial task, can enter idle,
    /// and can later resume from follow-up prompts.
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
        void set_teammate_runtime_factory(TeammateRuntimeFactory factory);

        void register_runtime_notification_handler(std::string runtime_key, RuntimeNotificationHandler handler);
        void unregister_runtime_notification_handler(const std::string &runtime_key);

        /// Spawn a new teammate.
        [[nodiscard]]
        AgentSpawnResult spawn(const AgentSpawnRequest &request);

        /// Send a message to a specific agent by run_id. Delivers via the team mailbox.
        [[nodiscard]]
        auto send_message(const std::string &run_id, const std::string &from, const std::string &text) -> std::optional<std::string>;

        /// Send a message to an agent by name within the caller's team.
        [[nodiscard]]
        auto send_message_by_name(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text) -> std::optional<std::string>;

        /// Broadcast a message to all team members except the sender.
        auto broadcast_message(const std::string &team_id, const std::string &from, const std::string &text) -> std::optional<std::string>;

        /// Stop a running teammate.
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
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::AgentExecutionEnvironment;
    using orchestration::OrchestrationManager;
} // namespace orangutan
