#include "features/tools/runtime/runtime-loader.hpp"

#include "tools/registry/permissions.hpp"
#include "features/tools/script/script-loader.hpp"

#include <spdlog/spdlog.h>

namespace orangutan {

    namespace {

        bool should_expose_tool(const ToolPermissionSettings &settings, const std::string &name) {
            if (!is_tool_allowed(settings, name)) {
                return false;
            }
            if (name == "shell" && settings.shell_approval == ToolApprovalPolicy::deny) {
                return false;
            }
            return true;
        }

        void apply_permission_policy(ToolRegistry &registry, const ToolPermissionSettings &settings, const ToolRuntimeContext *tool_context,
                                     ToolApprovalCallback approval_callback) {
            registry.set_definition_filter([settings](const ToolDef &definition) {
                return should_expose_tool(settings, definition.name);
            });
            registry.set_execution_guard([settings, approval_callback = std::move(approval_callback), tool_context](const ToolUse &call) {
                const auto &active_callback = tool_context != nullptr && tool_context->approval_callback ? tool_context->approval_callback : approval_callback;
                return evaluate_tool_permission(call, settings, active_callback);
            });
        }

    } // namespace

    RuntimeToolBootstrapResult register_runtime_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory, const std::string &workspace, const ToolRuntimeContext *tool_context,
                                                      const std::vector<Config::ScriptToolConfig> &custom_tools, const std::vector<Config::McpServerConfig> &mcp_servers,
                                                      const ToolPermissionSettings *permissions, ToolApprovalCallback approval_callback, std::string_view edit_mode) {
        register_builtin_tools(registry, runtime_memory, workspace, tool_context, permissions, edit_mode);
        register_script_tools(registry, custom_tools, workspace, permissions, tool_context, approval_callback);

        if (permissions != nullptr) {
            apply_permission_policy(registry, *permissions, tool_context, std::move(approval_callback));
        }

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
