#pragma once

#include "tools/registry/tool-registry.hpp"

#include <string>

namespace orangutan::tools {

    void register_message_attachments_tool(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context);

} // namespace orangutan::tools
