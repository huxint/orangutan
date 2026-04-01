#pragma once

#include "tools/registry/permissions.hpp"
#include "tools/registry/tool.hpp"

#include <string>

namespace orangutan::tools::shell {

    void register_tools(ToolRegistry &registry, const std::string &workspace = {}, const ToolRuntimeContext *tool_context = nullptr,
                        const ToolPermissionSettings *permissions = nullptr);

} // namespace orangutan::tools::shell
