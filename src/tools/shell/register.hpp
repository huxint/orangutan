#pragma once

#include "permissions/permission-types.hpp"
#include "tools/registry/tool.hpp"

#include <filesystem>

namespace orangutan::tools::shell {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root = {}, const ToolRuntimeContext *tool_context = nullptr,
                        const ToolPermissionContext *permissions = nullptr);

} // namespace orangutan::tools::shell
