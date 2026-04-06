#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools {

    struct ToolRuntimeContext;

    void register_agent_spawn_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_send_message_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_stop_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

    void register_coordinator_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
