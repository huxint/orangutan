#include "features/tools/core/internal.hpp"
#include "features/tools/core/background-completion.hpp"
#include "features/tools/builtin/register-builtin.hpp"

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

} // namespace orangutan
