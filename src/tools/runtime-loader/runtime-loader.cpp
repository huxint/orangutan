#include "tools/runtime-loader/runtime-loader.hpp"

#include "permissions/permission-evaluator.hpp"
#include "permissions/rule-parser.hpp"
#include "coordinator/coordinator-mode.hpp"
#include "tools/register.hpp"
#include "tools/script/register.hpp"
#include "tools/tool-search/tool-search.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::tools {

    namespace {

        void apply_permission_policy(ToolRegistry &registry, const ToolPermissionContext &ctx, const ToolRuntimeContext *tool_context) {
            registry.set_definition_filter([ctx](const ToolDef &definition) {
                for (const auto &rule : ctx.deny_rules) {
                    if (!rule.content && matches_rule(rule, definition.name)) {
                        return false;
                    }
                }
                return true;
            });
            registry.set_execution_guard([ctx, tool_context](const ToolUse &call) -> std::optional<ToolResult> {
                auto decision = evaluate_permission(call, ctx);
                decision = apply_post_processing(decision, ctx.mode);

                switch (decision.behavior) {
                case PermissionBehavior::allow:
                    return std::nullopt;
                case PermissionBehavior::deny:
                    return ToolResult{call.id, decision.message.value_or("Blocked by permission policy"), true};
                case PermissionBehavior::ask: {
                    const auto &callback = (tool_context && tool_context->approval_callback) ? tool_context->approval_callback : ApprovalCallback{};
                    if (!callback) {
                        return ToolResult{call.id, "Requires approval but interactive approval unavailable", true};
                    }
                    if (!callback(call, decision)) {
                        return ToolResult{call.id, "Rejected by user", true};
                    }
                    return std::nullopt;
                }
                }
                return std::nullopt;
            });
        }

    } // namespace

    RuntimeToolBootstrapResult register_runtime_tools(ToolRegistry &registry, memory::RuntimeMemory *runtime_memory, const std::string &workspace,
                                                      const ToolRuntimeContext *tool_context, const std::vector<Config::ScriptToolConfig> &custom_tools,
                                                      const std::vector<Config::McpServerConfig> &mcp_servers, const ToolPermissionContext *permissions,
                                                      std::string_view edit_mode) {
        const bool coordinator_only = coordinator::is_coordinator_mode(tool_context);
        register_builtin_tools(registry, runtime_memory, workspace, tool_context, permissions, edit_mode);
        if (!coordinator_only) {
            script::register_tools(registry, custom_tools, workspace, permissions, tool_context);
        }

        if (permissions != nullptr) {
            apply_permission_policy(registry, *permissions, tool_context);
        }

        RuntimeToolBootstrapResult result;
        if (!coordinator_only && !mcp_servers.empty()) {
            result.mcp_manager = std::make_unique<McpManager>(mcp_servers);
            result.mcp_manager->connect_all();
            result.mcp_manager->register_tools(registry);
            result.mcp_tool_count = result.mcp_manager->total_tool_count();

            spdlog::info("Registered {} MCP tool(s) across {} connected server(s)", result.mcp_tool_count, result.mcp_manager->connected_server_count());
        }

        // Register tool_search if there are any deferred tools (builtin or MCP)
        if (!coordinator_only && registry.has_deferred_tools()) {
            register_tool_search(registry);
            spdlog::debug("Registered tool_search for deferred tools");
        }

        return result;
    }

} // namespace orangutan::tools
