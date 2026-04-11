#pragma once

#include "tools/registry/tool-registry.hpp"

#include <filesystem>

namespace orangutan::tools {

    // NOLINTNEXTLINE(readability-redundant-declaration)
    void register_message_attachments_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
