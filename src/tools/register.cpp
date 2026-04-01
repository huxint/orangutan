#include "tools/register.hpp"

#include "tools/background/background-completion.hpp"
#include "tools/internal.hpp"

#include <filesystem>

namespace orangutan {

    void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context, const ToolPermissionSettings *permissions,
                                     std::string_view edit_mode) {
        const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
        const auto completion_dispatcher = std::make_shared<BackgroundCompletionDispatcher>(tool_context);
        const auto process_manager = std::make_shared<BackgroundProcessManager>([completion_dispatcher](const BackgroundProcessCompletionEvent &event) {
            completion_dispatcher->dispatch(event);
        });
        register_shell_tool(registry, workspace, permissions, completion_dispatcher, process_manager);
        register_process_tools(registry, process_manager);
        register_read_tool(registry, workspace_root, edit_mode);
        register_write_tool(registry, workspace_root);
        register_edit_tool(registry, workspace_root, edit_mode);
    }

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
