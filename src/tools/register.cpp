#include "tools/register.hpp"

#include "tools/coordinator/register.hpp"
#include "tools/file-edit/register.hpp"
#include "tools/file-read/register.hpp"
#include "tools/file-write/register.hpp"
#include "tools/heartbeat/register.hpp"
#include "tools/inbox/register.hpp"
#include "tools/message-attachments/message-attachments-tool.hpp"
#include "tools/memory/register.hpp"
#include "tools/shell/register.hpp"
#include "tools/swarm/register.hpp"
#include "tools/task/register.hpp"
#include "tools/registry/tool-context.hpp"

#include <filesystem>

namespace orangutan::tools {

    void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context, const ToolPermissionSettings *permissions,
                                     std::string_view edit_mode) {
        const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
        shell::register_tools(registry, workspace, tool_context, permissions);
        file_read::register_tools(registry, workspace_root, edit_mode);
        file_write::register_tools(registry, workspace_root);
        file_edit::register_tools(registry, workspace_root, edit_mode);
    }

    void register_builtin_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory *runtime_memory, const std::string &workspace, const ToolRuntimeContext *tool_context,
                                const ToolPermissionSettings *permissions, std::string_view edit_mode) {
        register_builtin_core_tools(registry, workspace, tool_context, permissions, edit_mode);
        task::register_tools(registry, tool_context);
        heartbeat::register_tools(registry, tool_context);
        inbox::register_tools(registry, tool_context);
        register_message_attachments_tool(registry, workspace, tool_context);
        // Register coordinator/swarm tools if coordinator_manager is available
        if (tool_context != nullptr && tool_context->coordinator_manager != nullptr && !tool_context->is_child_run) {
            if (tool_context->coordinator_mode) {
                // In coordinator mode, only register orchestration tools
                register_coordinator_tools(registry, tool_context);
            } else if (!tool_context->team_agents.empty()) {
                // Non-coordinator agents with team_agents can spawn and communicate
                register_coordinator_tools(registry, tool_context);
                register_swarm_tools(registry, tool_context);
            }
        }
        if (runtime_memory != nullptr) {
            memory::register_tools(registry, *runtime_memory);
        }
    }

} // namespace orangutan::tools
