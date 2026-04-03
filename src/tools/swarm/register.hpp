#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools {

    struct ToolRuntimeContext;

    void register_team_create_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_team_delete_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

    void register_swarm_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
