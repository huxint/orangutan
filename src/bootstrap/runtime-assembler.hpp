#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "orchestration/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig;

    struct RuntimeAssemblyRequest {
        const AgentRuntimeConfig *runtime_config = nullptr;
        const RuntimeIdentity *identity = nullptr;
        const Config *app_config = nullptr;

        memory::MemoryStore *memory_store = nullptr;
        std::string agent_name;
        std::string *current_session_id = nullptr;
        std::optional<std::vector<std::string>> team_agents;
        std::string team_id;
        orchestration::OrchestrationManager *orchestration_manager = nullptr;
        orchestration::TeamManager *team_manager = nullptr;
        orchestration::AgentMailbox *mailbox = nullptr;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id = "cli:local";
        automation::AutomationService *automation_service = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
        bool is_child_run = false; // NOLINT: retained for backward compat, prefer agent_role
        std::optional<bool> coordinator_mode; // NOLINT: retained for backward compat, prefer agent_role
        orchestration::agent_role agent_role = orchestration::agent_role::standalone;
        RuntimeAbortChecker abort_checker;
        ApprovalCallback approval_callback;
        std::string delegated_task_prompt;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
    };

    [[nodiscard]]
    AgentRuntimeBuildInput make_runtime_build_input(const RuntimeAssemblyRequest &request);

} // namespace orangutan::bootstrap
