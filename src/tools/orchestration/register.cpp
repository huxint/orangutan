#include "tools/orchestration/register.hpp"

#include "tools/registry/tool-context.hpp"

namespace orangutan::tools {

    void register_orchestration_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_orchestration_tools(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_orchestration_tools(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability) {
        register_agent_spawn_tool(registry, identity, capability);
        register_agent_send_message_tool(registry, identity, capability);
        register_agent_stop_tool(registry, identity, capability);
        register_team_create_tool(registry, identity, capability);
        register_team_delete_tool(registry, identity, capability);
    }

    void register_agent_communication_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_agent_communication_tools(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_agent_communication_tools(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability) {
        register_agent_send_message_tool(registry, identity, capability);
    }

} // namespace orangutan::tools
