#pragma once

#include "tools/registry/tool.hpp"

namespace orangutan::tools::heartbeat {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools::heartbeat
