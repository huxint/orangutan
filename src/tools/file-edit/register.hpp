#pragma once

#include "tools/registry/tool.hpp"

#include <filesystem>
#include <string_view>

namespace orangutan::tools::file_edit {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools::file_edit
