#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"

namespace orangutan::tools {

    void register_swarm_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        register_team_create_tool(registry, tool_context);
        register_team_delete_tool(registry, tool_context);
    }

} // namespace orangutan::tools
