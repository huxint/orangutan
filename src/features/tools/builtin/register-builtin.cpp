#include "features/tools/builtin/register-builtin.hpp"
#include "core/tools/tool.hpp"

namespace orangutan {

void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory, const std::string &workspace,
                            const ToolRuntimeContext *tool_context, const ToolPermissionSettings *permissions) {
    register_builtin_core_tools(registry, workspace, permissions);
    register_builtin_subagent_tools(registry, tool_context);
    register_cron_tool(registry, tool_context);
    if (runtime_memory != nullptr) {
        register_builtin_memory_tools(registry, *runtime_memory);
    }
}

} // namespace orangutan
