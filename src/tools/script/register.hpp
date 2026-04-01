#pragma once

#include "tools/script/script-loader.hpp"

namespace orangutan::tools::script {

    void register_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace = {}, const ToolPermissionSettings *permissions = nullptr,
                        const ToolRuntimeContext *tool_context = nullptr, const ToolApprovalCallback &approval_callback = {});

} // namespace orangutan::tools::script
