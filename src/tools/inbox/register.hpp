#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools::inbox {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools::inbox
