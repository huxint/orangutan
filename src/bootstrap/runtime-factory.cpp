#include "bootstrap/runtime-factory.hpp"

#include "bootstrap/runtime-assembler.hpp"

namespace orangutan::bootstrap {

    std::vector<std::string> make_fallback_model_labels(std::span<const config::FallbackModelRef> fallback_models) {
        std::vector<std::string> labels;
        labels.reserve(fallback_models.size());
        for (const auto &fallback : fallback_models) {
            if (fallback.profile.empty()) {
                labels.push_back(fallback.model);
            } else {
                labels.push_back(fallback.profile + ":" + fallback.model);
            }
        }
        return labels;
    }

    AgentRuntimeConfig make_agent_runtime_config(std::string agent_key, const config::AgentConfig &agent_cfg, providers::ProviderRoute provider_route, std::string workspace_root,
                                                 ToolPermissionContext permission_context, std::string api_key_override) {
        const auto identity = derive_cli_identity(workspace_root, agent_key);
        auto runtime_workspace_root = std::move(workspace_root);
        return AgentRuntimeConfig{
            .agent_key = std::move(agent_key),
            .model = agent_cfg.model,
            .fallback_models = make_fallback_model_labels(agent_cfg.fallback_models),
            .provider_route = std::move(provider_route),
            .api_key_override = std::move(api_key_override),
            .workspace_root = std::move(runtime_workspace_root),
            .thinking_budget = agent_cfg.thinking_budget,
            .cli_runtime_key = identity.runtime_key,
            .cli_memory_scope = identity.memory_scope,
            .permission_context = std::move(permission_context),
            .leader_mode = agent_cfg.leader_mode,
            .max_concurrent_agents = agent_cfg.max_concurrent_agents,
        };
    }

    AgentRuntimeBundle build_runtime_bundle(const RuntimeFactoryRequest &request) {
        return build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
            .runtime_config = request.runtime_config,
            .identity = request.identity,
            .app_config = request.app_config,
            .memory_store = request.memory_store,
            .agent_name = request.agent_name,
            .current_session_id = request.current_session_id,
            .team_id = request.team_id,
            .orchestration_manager = request.orchestration_manager,
            .team_manager = request.team_manager,
            .mailbox = request.mailbox,
            .runtime_origin = request.runtime_origin,
            .raw_caller_id = request.raw_caller_id,
            .automation_service = request.automation_service,
            .automation_runtime = request.automation_runtime,
            .agent_role = request.agent_role,
            .abort_checker = request.abort_checker,
            .approval_callback = request.approval_callback,
            .delegated_task_prompt = request.delegated_task_prompt,
            .hook_manager = request.hook_manager,
            .background_completion_runtime = request.background_completion_runtime,
        }));
    }

} // namespace orangutan::bootstrap
