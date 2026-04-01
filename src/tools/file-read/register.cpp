#include "tools/file-read/register.hpp"

#include "tools/internal.hpp"

namespace orangutan::tools::file_read {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, std::string_view edit_mode) {
        register_read_tool(registry, workspace_root, edit_mode);
    }

} // namespace orangutan::tools::file_read
