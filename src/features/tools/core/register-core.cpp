#include "features/tools/core/internal.hpp"
#include "features/tools/builtin/register-builtin.hpp"

#include <filesystem>

namespace orangutan {

void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace, const ToolPermissionSettings *permissions) {
    const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
    register_shell_tool(registry, workspace, permissions);
    register_read_tool(registry, workspace_root);
    register_write_tool(registry, workspace_root);
    register_edit_tool(registry, workspace_root);
}

} // namespace orangutan
