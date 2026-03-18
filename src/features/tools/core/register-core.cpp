#include "features/tools/core/internal.hpp"
#include "features/tools/builtin/register-builtin.hpp"

#include <filesystem>

namespace orangutan {

void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace, const ToolPermissionSettings *permissions) {
    const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
    const auto process_manager = std::make_shared<BackgroundProcessManager>();
    register_shell_tool(registry, workspace, permissions, process_manager);
    register_process_tools(registry, process_manager);
    register_read_tool(registry, workspace_root);
    register_write_tool(registry, workspace_root);
    register_edit_tool(registry, workspace_root);
}

} // namespace orangutan
