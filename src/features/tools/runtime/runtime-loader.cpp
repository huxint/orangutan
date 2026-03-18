#include "features/tools/runtime/runtime-loader.hpp"

#include "features/tools/script/script-loader.hpp"

#include <spdlog/spdlog.h>

namespace orangutan {

RuntimeToolBootstrapResult register_runtime_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory, const std::string &workspace,
                                                  const ToolRuntimeContext *tool_context, const std::vector<Config::ScriptToolConfig> &custom_tools,
                                                  const std::vector<Config::McpServerConfig> &mcp_servers) {
    register_builtin_tools(registry, runtime_memory, workspace, tool_context);
    register_script_tools(registry, custom_tools, workspace);

    RuntimeToolBootstrapResult result;
    if (mcp_servers.empty()) {
        return result;
    }

    result.mcp_manager = std::make_unique<McpManager>(mcp_servers);
    result.mcp_manager->connect_all();
    result.mcp_manager->register_tools(registry);
    result.mcp_tool_count = result.mcp_manager->total_tool_count();

    spdlog::info("Registered {} MCP tool(s) across {} connected server(s)", result.mcp_tool_count, result.mcp_manager->connected_server_count());
    return result;
}

} // namespace orangutan
