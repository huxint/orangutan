#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools {

    // NOLINTNEXTLINE(readability-redundant-declaration)
    void register_task_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
