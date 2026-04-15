#include "bootstrap/runtime-assembler.hpp"

#include "bootstrap/channel-serve.hpp"

namespace orangutan::bootstrap {

    namespace {

        std::vector<std::string> resolve_team_agents(const RuntimeAssemblyRequest &request) {
            if (request.team_agents.has_value()) {
                return *request.team_agents;
            }
            return request.runtime_config->team_agents;
        }

        bool resolve_coordinator_mode(const RuntimeAssemblyRequest &request) {
            if (request.coordinator_mode.has_value()) {
                return *request.coordinator_mode;
            }
            return request.runtime_config->coordinator_mode;
        }

    } // namespace

    AgentRuntimeBuildInput make_runtime_build_input(const RuntimeAssemblyRequest &request) {
        return AgentRuntimeBuildInput{
            .provider_route = request.runtime_config->provider_route,
            .agent_key = request.runtime_config->agent_key,
            .agent_name = request.agent_name.empty() ? request.runtime_config->agent_key : request.agent_name,
            .workspace_root = request.runtime_config->workspace_root,
            .edit_mode = request.runtime_config->edit_mode,
            .thinking_budget = request.runtime_config->thinking_budget,
            .memory = request.runtime_config->memory,
            .permission_context = request.runtime_config->permission_context,
            .team_agents = resolve_team_agents(request),
            .team_id = request.team_id,
            .identity = *request.identity,
            .memory_store = request.memory_store,
            .current_session_id = request.current_session_id,
            .coordinator_manager = request.coordinator_manager,
            .team_manager = request.team_manager,
            .mailbox = request.mailbox,
            .runtime_origin = request.runtime_origin,
            .raw_caller_id = request.raw_caller_id,
            .automation_service = request.automation_service,
            .automation_runtime = request.automation_runtime,
            .is_child_run = request.is_child_run,
            .coordinator_mode = resolve_coordinator_mode(request),
            .abort_checker = request.abort_checker,
            .approval_callback = request.approval_callback,
            .delegated_task_prompt = request.delegated_task_prompt,
            .custom_tools = request.app_config->custom_tools,
            .mcp_servers = request.app_config->mcp_servers,
            .skill_paths = request.app_config->skill_paths,
            .hook_paths = request.app_config->hook_paths,
            .background_completion_runtime = request.background_completion_runtime,
        };
    }

} // namespace orangutan::bootstrap
