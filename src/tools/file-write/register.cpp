#include "tools/file-write/register.hpp"

#include "tools/internal.hpp"

namespace orangutan::tools::file_write {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root) {
        register_write_tool(registry, workspace_root);
    }

} // namespace orangutan::tools::file_write
