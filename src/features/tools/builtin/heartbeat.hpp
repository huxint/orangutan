#pragma once

#include "core/tools/tool.hpp"

namespace orangutan {

void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan
