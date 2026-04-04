#include "tools/file-edit/register.hpp"

#include "tools/internal.hpp"

namespace orangutan::tools::file_edit {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions, std::string_view edit_mode) {
        register_edit_tool(registry, workspace_root, permissions, edit_mode);
    }

} // namespace orangutan::tools::file_edit
