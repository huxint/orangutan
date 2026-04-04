#include "tools/file-write/register.hpp"

#include "tools/internal.hpp"

namespace orangutan::tools::file_write {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        register_write_tool(registry, workspace_root, permissions);
    }

} // namespace orangutan::tools::file_write
