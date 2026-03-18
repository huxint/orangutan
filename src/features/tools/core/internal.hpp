#pragma once

#include "core/tools/tool.hpp"

#include <filesystem>
#include <string>

namespace orangutan {

inline std::filesystem::path resolve_tool_path(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
    if (path.is_absolute() || workspace_root.empty()) {
        return path;
    }

    return workspace_root / path;
}

void register_shell_tool(ToolRegistry &registry, const std::string &workspace);
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);
void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);

} // namespace orangutan
