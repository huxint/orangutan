#pragma once

#include "tools/script/script-loader.hpp"

namespace orangutan::tools::script {

    void register_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace = {}, const ToolPermissionContext *permissions = nullptr,
                        const ToolRuntimeContext *tool_context = nullptr, const ApprovalCallback &approval_callback = {});

} // namespace orangutan::tools::script
