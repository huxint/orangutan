#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "types/base.hpp"

namespace orangutan::orchestration {

    /// Unified internal agent role within the orchestration hierarchy.
    enum class agent_role : std::uint8_t {
        /// Standalone agent — no orchestration context.
        standalone,
        /// Orchestrator that plans, delegates, and synthesizes work.
        leader,
        /// Persistent teammate: runs tasks, then idles waiting for follow-up messages.
        teammate,
    };

    enum class teammate_relationship : std::uint8_t {
        managed,
        peer,
    };

    [[nodiscard]]
    constexpr auto is_leader(agent_role role) -> bool {
        return role == agent_role::leader;
    }

    [[nodiscard]]
    constexpr auto is_teammate(agent_role role) -> bool {
        return role == agent_role::teammate;
    }

    /// Status of an agent run within the orchestration system.
    enum class run_status : std::uint8_t {
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
        std::string agent_name;
        std::string team_id;
        std::string parent_runtime_key;
        agent_role role = agent_role::standalone;
        teammate_relationship relationship = teammate_relationship::managed;
        run_status status = run_status::queued;
        std::string task_summary;
        std::string final_output;
        std::string error;
        std::int64_t started_at = 0;
        std::int64_t completed_at = 0;
    };

    /// Request to spawn a new agent within the orchestration system.
    struct AgentSpawnRequest {
        std::string name;
        std::string instructions;
        std::string task;
        std::string team_id;
        std::string parent_runtime_key;
        std::string config_agent_key;
        std::string profile_override;
        std::string model_override;
        int thinking_budget_override = 0;
        teammate_relationship relationship = teammate_relationship::managed;
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
    using orchestration::teammate_relationship;
    using orchestration::run_status;
    using orchestration::RuntimeNotificationHandler;
    using orchestration::TaskNotificationCallback;
} // namespace orangutan
