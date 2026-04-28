#include "bootstrap/runtime-assembler.hpp"

namespace orangutan::bootstrap {

    namespace {

        auto resolve_agent_role(const RuntimeAssemblyRequest &request) -> orchestration::agent_role {
            if (request.agent_role != orchestration::agent_role::standalone) {
                return request.agent_role;
            }
            return request.runtime_config->leader_mode ? orchestration::agent_role::leader : orchestration::agent_role::standalone;
        }

    } // namespace

    AgentRuntimeBuildInput make_runtime_build_input(const RuntimeAssemblyRequest &request) {
        return AgentRuntimeBuildInput{
            .provider_route = request.runtime_config->provider_route,
            .agent_key = request.runtime_config->agent_key,
            .agent_name = request.agent_name.empty() ? request.runtime_config->agent_key : request.agent_name,
            .workspace_root = request.runtime_config->workspace_root,
            .thinking_budget = request.runtime_config->thinking_budget,
            .permission_context = request.runtime_config->permission_context,
            .team_id = request.team_id,
            .identity = *request.identity,
            .memory_store = request.memory_store,
            .current_session_id = request.current_session_id,
            .orchestration_manager = request.orchestration_manager,
            .team_manager = request.team_manager,
            .mailbox = request.mailbox,
            .runtime_origin = request.runtime_origin,
            .raw_caller_id = request.raw_caller_id,
            .automation_service = request.automation_service,
            .automation_runtime = request.automation_runtime,
            .agent_role = resolve_agent_role(request),
            .abort_checker = request.abort_checker,
            .approval_callback = request.approval_callback,
            .delegated_task_prompt = request.delegated_task_prompt,
            .custom_tools = request.app_config->custom_tools,
            .mcp_servers = request.app_config->mcp_servers,
            .skill_paths = request.app_config->skill_paths,
            .hook_paths = request.app_config->hook_paths,
            .hook_manager = request.hook_manager,
            .background_completion_runtime = request.background_completion_runtime,
        };
    }

} // namespace orangutan::bootstrap
