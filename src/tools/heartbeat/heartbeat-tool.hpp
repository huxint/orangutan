#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools {

    void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
