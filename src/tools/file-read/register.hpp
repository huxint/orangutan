#pragma once

#include "tools/registry/tool-registry.hpp"

#include <filesystem>
#include <string_view>

namespace orangutan::tools::file_read {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr,
                        std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools::file_read
