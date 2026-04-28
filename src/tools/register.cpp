#include "tools/register.hpp"

#include "tools/automation/automation-tool.hpp"
#include "orchestration/leader-mode.hpp"
#include "tools/internal.hpp"
#include "tools/message-attachments/message-attachments-tool.hpp"
#include "tools/orchestration/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/shell/register.hpp"

#include <filesystem>

namespace orangutan::tools {

    void register_builtin_core_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context,
                                     const ToolPermissionContext *permissions) {
        shell::register_tools(registry, workspace_root, tool_context, permissions);
        register_read_tool(registry, workspace_root, permissions);
        register_write_tool(registry, workspace_root, permissions);
        register_edit_tool(registry, workspace_root, permissions);
        register_fd_tool(registry, workspace_root, permissions);
        register_rg_tool(registry, workspace_root, permissions);
    }

    void register_builtin_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory *runtime_memory, const std::filesystem::path &workspace_root,
                                const ToolRuntimeContext *tool_context, const ToolPermissionContext *permissions) {
        // Leader mode: only orchestration tools (spawn, send, stop, team management)
        if (orchestration::is_leader_mode(tool_context)) {
            register_orchestration_tools(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
            return;
        }

        register_builtin_core_tools(registry, workspace_root, tool_context, permissions);
        if (tool_context != nullptr) {
            register_automation_tool(registry, automation_capability(*tool_context));
            register_message_attachments_tool(registry, workspace_root, tool_context);
        }

        if (tool_context != nullptr && tool_context->orchestration_manager != nullptr && orchestration::is_teammate(tool_context->role)) {
            register_agent_communication_tools(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
        }

        const bool has_orchestration = tool_context != nullptr && tool_context->orchestration_manager != nullptr && !orchestration::is_teammate(tool_context->role);
        if (has_orchestration) {
            register_orchestration_tools(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
        }

        if (runtime_memory != nullptr) {
            register_builtin_memory_tools(registry, *runtime_memory);
        }
    }

} // namespace orangutan::tools
