#include "tools/register.hpp"

#include "tools/automation/automation-tool.hpp"
#include "coordinator/coordinator-mode.hpp"
#include "tools/coordinator/register.hpp"
#include "tools/internal.hpp"
#include "tools/message-attachments/message-attachments-tool.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/shell/register.hpp"
#include "tools/swarm/register.hpp"

#include <filesystem>

namespace orangutan::tools {

    void register_builtin_core_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context,
                                     const ToolPermissionContext *permissions, std::string_view edit_mode) {
        shell::register_tools(registry, workspace_root, tool_context, permissions);
        register_read_tool(registry, workspace_root, permissions, edit_mode);
        register_write_tool(registry, workspace_root, permissions);
        register_edit_tool(registry, workspace_root, permissions, edit_mode);
        register_fs_tools(registry, workspace_root, permissions);
        register_fd_tool(registry, workspace_root, permissions);
        register_rg_tool(registry, workspace_root, permissions);
    }

    void register_builtin_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory *runtime_memory, const std::filesystem::path &workspace_root,
                                const ToolRuntimeContext *tool_context, const ToolPermissionContext *permissions, std::string_view edit_mode) {
        if (coordinator::is_coordinator_mode(tool_context)) {
            register_coordinator_tools(registry, tool_context);
            registry.discover_deferred_tools();
            return;
        }

        register_builtin_core_tools(registry, workspace_root, tool_context, permissions, edit_mode);
        register_automation_tool(registry, tool_context);
        register_message_attachments_tool(registry, workspace_root, tool_context);

        const bool can_register_collaboration_tools = tool_context != nullptr && tool_context->coordinator_manager != nullptr && !tool_context->is_child_run;
        if (can_register_collaboration_tools && !tool_context->team_agents.empty()) {
            // Team workers can orchestrate sub-agents and manage their team lifecycle.
            register_coordinator_tools(registry, tool_context);
            register_swarm_tools(registry, tool_context);
        }

        if (runtime_memory != nullptr) {
            register_builtin_memory_tools(registry, *runtime_memory);
        }
    }

} // namespace orangutan::tools
