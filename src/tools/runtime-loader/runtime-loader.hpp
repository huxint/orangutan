#pragma once

#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "tools/mcp/mcp-manager.hpp"
#include "tools/registry/tool.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace orangutan::tools {

    struct RuntimeToolBootstrapResult {
        std::unique_ptr<McpManager> mcp_manager;
        std::size_t mcp_tool_count = 0;
    };

    RuntimeToolBootstrapResult register_runtime_tools(ToolRegistry &registry, memory::RuntimeMemory *runtime_memory, const std::string &workspace,
                                                      const ToolRuntimeContext *tool_context, const std::vector<Config::ScriptToolConfig> &custom_tools,
                                                      const std::vector<Config::McpServerConfig> &mcp_servers, const ToolPermissionContext *permissions = nullptr,
                                                      std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools
