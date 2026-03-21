#include "features/tools/builtin/register-builtin.hpp"
#include "core/tools/tool.hpp"

namespace orangutan {

void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory, const std::string &workspace, const ToolRuntimeContext *tool_context,
                            const ToolPermissionSettings *permissions, std::string_view edit_mode) {
    register_builtin_core_tools(registry, workspace, tool_context, permissions, edit_mode);
    register_builtin_subagent_tools(registry, tool_context);
    register_task_tool(registry, tool_context);
    register_heartbeat_tool(registry, tool_context);
    register_inbox_tool(registry, tool_context);
    if (runtime_memory != nullptr) {
        register_builtin_memory_tools(registry, *runtime_memory);
    }
}

} // namespace orangutan
