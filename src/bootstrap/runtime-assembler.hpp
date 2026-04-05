#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"

#include <optional>
#include <string>
#include <vector>

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig;

    struct RuntimeAssemblyRequest {
        const AgentRuntimeConfig &runtime_config;
        const RuntimeIdentity &identity;
        const Config &app_config;

        memory::MemoryStore *memory_store = nullptr;
        std::string agent_name;
        std::string *current_session_id = nullptr;
        std::optional<std::vector<std::string>> team_agents;
        std::string team_id;
        coordinator::CoordinatorManager *coordinator_manager = nullptr;
        swarm::TeamManager *team_manager = nullptr;
        swarm::AgentMailbox *mailbox = nullptr;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id = "cli:local";
        automation::Runtime *automation_runtime = nullptr;
        bool is_child_run = false;
        std::optional<bool> coordinator_mode;
        RuntimeAbortChecker abort_checker;
        ApprovalCallback approval_callback;
        std::string delegated_task_prompt;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
    };

    [[nodiscard]]
    AgentRuntimeBuildInput make_runtime_build_input(const RuntimeAssemblyRequest &request);

} // namespace orangutan::bootstrap

