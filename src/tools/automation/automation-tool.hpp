#pragma once

#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools {

    void register_automation_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
