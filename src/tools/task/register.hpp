#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools::task {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools::task
