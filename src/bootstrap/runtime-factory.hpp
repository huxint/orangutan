#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "bootstrap/runtime-config.hpp"
#include "config/config.hpp"
#include "orchestration/types.hpp"
#include "providers/provider.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace orangutan::automation {
    class AutomationRuntime;
    class AutomationService;
} // namespace orangutan::automation

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::orchestration {
    class OrchestrationManager;
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::orchestration

namespace orangutan::bootstrap {

    struct RuntimeFactoryRequest {
        const AgentRuntimeConfig *runtime_config = nullptr;
        const RuntimeIdentity *identity = nullptr;
        const config::Config *app_config = nullptr;

        memory::MemoryStore *memory_store = nullptr;
        std::string agent_name;
        std::string *current_session_id = nullptr;
        std::string team_id;
        orchestration::OrchestrationManager *orchestration_manager = nullptr;
        orchestration::TeamManager *team_manager = nullptr;
        orchestration::AgentMailbox *mailbox = nullptr;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id = "cli:local";
        automation::AutomationService *automation_service = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
        orchestration::agent_role agent_role = orchestration::agent_role::standalone;
        RuntimeAbortChecker abort_checker;
        ApprovalCallback approval_callback;
        std::string delegated_task_prompt;
        hooks::HookManager *hook_manager = nullptr;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
    };

    [[nodiscard]]
    std::vector<std::string> make_fallback_model_labels(std::span<const config::FallbackModelRef> fallback_models);

    [[nodiscard]]
    AgentRuntimeConfig make_agent_runtime_config(std::string agent_key, const config::AgentConfig &agent_cfg, providers::ProviderRoute provider_route,
                                                 std::string workspace_root, ToolPermissionContext permission_context, std::string api_key_override = {});

    [[nodiscard]]
    AgentRuntimeBundle build_runtime_bundle(const RuntimeFactoryRequest &request);

} // namespace orangutan::bootstrap
