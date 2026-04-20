#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "types/base.hpp"

namespace orangutan::orchestration {

    /// Unified agent role within the orchestration hierarchy.
    /// Replaces the legacy `coordinator_mode` / `is_child_run` bool pair.
    enum class agent_role : base::u8 {
        /// Standalone agent — no orchestration context.
        standalone,
        /// Orchestrator that spawns and manages workers.
        leader,
        /// Fire-and-forget worker: runs one task, reports, exits.
        worker,
        /// Persistent worker: runs tasks, then idles waiting for follow-up messages.
        teammate,
    };

    /// Status of an agent run within the orchestration system.
    enum class run_status : base::u8 {
        queued,
        running,
        idle,      ///< Teammate finished a task, waiting for next prompt
        succeeded,
        failed,
        terminated,
        abandoned,
    };

    /// Record tracking the lifecycle of a single agent run.
    struct AgentRunRecord {
        std::string run_id;
        std::string agent_key;
        std::string agent_name;
        std::string team_id;
        std::string parent_runtime_key;
        agent_role role = agent_role::standalone;
        run_status status = run_status::queued;
        std::string task_summary;
        std::string final_output;
        std::string error;
        std::int64_t started_at = 0;
        std::int64_t completed_at = 0;
    };

    /// Request to spawn a new agent within the orchestration system.
    struct AgentSpawnRequest {
        std::string agent_key;
        std::string agent_name;
        std::string task_prompt;
        std::string team_id;
        std::string parent_runtime_key;
        std::string workspace_root;
        /// Determines the spawned agent's lifecycle:
        /// - agent_role::worker: run once, report, exit
        /// - agent_role::teammate: run, idle, wait for follow-up messages
        agent_role role = agent_role::worker;
    };

    /// Result of a spawn attempt.
    struct AgentSpawnResult {
        bool accepted = false;
        std::string run_id;
        std::string agent_name;
        std::string error;
    };

    /// Callback to inject <task-notification> into the leader's conversation.
    using TaskNotificationCallback = std::function<void(const AgentRunRecord &record)>;

    /// Handler for runtime-scoped notification delivery.
    using RuntimeNotificationHandler = std::function<std::optional<std::string>(const std::string &message)>;

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::agent_role;
    using orchestration::AgentRunRecord;
    using orchestration::AgentSpawnRequest;
    using orchestration::AgentSpawnResult;
    using orchestration::run_status;
    using orchestration::RuntimeNotificationHandler;
    using orchestration::TaskNotificationCallback;
} // namespace orangutan
