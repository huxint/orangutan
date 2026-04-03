#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"

namespace orangutan::tools {

    void register_coordinator_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        register_agent_spawn_tool(registry, tool_context);
        register_agent_send_message_tool(registry, tool_context);
        register_agent_stop_tool(registry, tool_context);
    }

} // namespace orangutan::tools
