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
#include <thread>
#include <unordered_map>
#include <vector>
#include "types/base.hpp"

// Forward declarations
namespace orangutan::coordinator {
    class AgentDefinitionRegistry;
}
namespace orangutan::memory {
    class MemoryStore;
}
namespace orangutan::storage {
    class SessionStore;
}
namespace orangutan::swarm {
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::swarm

namespace orangutan::coordinator {

    enum class agent_run_status : base::u8 {
        queued,
        running,
        succeeded,
        failed,
        terminated,
        abandoned,
    };

    struct AgentRunRecord {
        std::string run_id;
        std::string agent_key;
        std::string agent_name;
        std::string team_id;
        std::string parent_runtime_key;
        agent_run_status status = agent_run_status::queued;
        std::string task_summary;
        std::string final_output;
        std::string error;
        std::int64_t started_at = 0;
        std::int64_t completed_at = 0;
    };

    struct AgentSpawnRequest {
        std::string agent_key;
        std::string agent_name; // human-readable label
        std::string task_prompt;
        std::string team_id; // optional
        std::string parent_runtime_key;
        std::string workspace_root;
    };

    struct AgentSpawnResult {
        bool accepted = false;
        std::string run_id;
        std::string agent_name;
        std::string error;
    };

    // Callback to inject <task-notification> into coordinator's conversation
    using TaskNotificationCallback = std::function<void(const AgentRunRecord &record)>;
    using RuntimeNotificationHandler = std::function<std::optional<std::string>(const std::string &message)>;

    struct WorkerRuntime {
        virtual ~WorkerRuntime() = default;
        WorkerRuntime() = default;
        WorkerRuntime(const WorkerRuntime &) = delete;
        WorkerRuntime &operator=(const WorkerRuntime &) = delete;
        WorkerRuntime(WorkerRuntime &&) = delete;
        WorkerRuntime &operator=(WorkerRuntime &&) = delete;
        virtual std::string run(const std::string &prompt, std::stop_token stop_token) = 0;
    };

    using WorkerRuntimeFactory = std::function<std::unique_ptr<WorkerRuntime>(const AgentSpawnRequest &request)>;

    // Environment for spawned agent execution
    struct AgentExecutionEnvironment {
        const AgentDefinitionRegistry *definition_registry = nullptr;
        storage::SessionStore *session_store = nullptr;
        memory::MemoryStore *memory_store = nullptr;
        swarm::AgentMailbox *mailbox = nullptr;
        swarm::TeamManager *team_manager = nullptr;
    };

    class CoordinatorManager {
    public:
        explicit CoordinatorManager(int max_concurrent_agents = 4);
        ~CoordinatorManager();

        CoordinatorManager(const CoordinatorManager &) = delete;
        CoordinatorManager &operator=(const CoordinatorManager &) = delete;
        CoordinatorManager(CoordinatorManager &&) = delete;
        CoordinatorManager &operator=(CoordinatorManager &&) = delete;

        void set_environment(AgentExecutionEnvironment env);
        void set_notification_callback(TaskNotificationCallback callback);
        void register_runtime_notification_handler(std::string runtime_key, RuntimeNotificationHandler handler);
        void unregister_runtime_notification_handler(const std::string &runtime_key);
        void set_worker_runtime_factory(WorkerRuntimeFactory factory);

        [[nodiscard]]
        AgentSpawnResult spawn(const AgentSpawnRequest &request);

        [[nodiscard]]
        std::optional<std::string> send_message(const std::string &run_id, const std::string &from, const std::string &text);

        void stop(const std::string &run_id);

        [[nodiscard]]
        std::optional<AgentRunRecord> get_run(const std::string &run_id) const;

        [[nodiscard]]
        std::vector<AgentRunRecord> list_active_runs() const;

        void shutdown();

    private:
        struct ActiveRun {
            AgentSpawnRequest request;
            AgentRunRecord record;
            std::stop_source stop_source;
            std::jthread worker_thread;
            mutable std::mutex mutex;
            std::condition_variable_any cv;
            bool completed = false;
        };

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

        [[nodiscard]]
        std::string make_run_id();

        [[nodiscard]]
        int count_running_locked() const;

        void launch_run_locked(const std::shared_ptr<ActiveRun> &run);
        void remove_pending_run_locked(const std::string &run_id);
        void maybe_start_queued_runs();
        void run_worker(const std::shared_ptr<ActiveRun> &run, const std::stop_token &stop_token);
    };

} // namespace orangutan::coordinator

namespace orangutan {
    using coordinator::agent_run_status;
    using coordinator::AgentExecutionEnvironment;
    using coordinator::AgentRunRecord;
    using coordinator::AgentSpawnRequest;
    using coordinator::AgentSpawnResult;
    using coordinator::CoordinatorManager;
    using coordinator::TaskNotificationCallback;
    using coordinator::WorkerRuntime;
    using coordinator::WorkerRuntimeFactory;
} // namespace orangutan
