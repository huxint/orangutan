#include "tools/script/register.hpp"

namespace orangutan::tools::script {

    void register_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace, const ToolPermissionContext *permissions,
                        const ToolRuntimeContext *tool_context, const ApprovalCallback &approval_callback) {
        register_script_tools(registry, tools, workspace, permissions, tool_context, approval_callback);
    }

} // namespace orangutan::tools::script
