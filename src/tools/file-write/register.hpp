#pragma once

#include "tools/registry/tool-registry.hpp"

#include <filesystem>

namespace orangutan::tools::file_write {

    void register_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr);

} // namespace orangutan::tools::file_write
