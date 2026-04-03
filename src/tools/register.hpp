#pragma once

#include "tools/registry/permissions.hpp"
#include "tools/registry/tool-registry.hpp"

#include <string>
#include <string_view>

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::tools {

    void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace = {}, const ToolRuntimeContext *tool_context = nullptr,
                                     const ToolPermissionSettings *permissions = nullptr, std::string_view edit_mode = "search_replace");
    void register_builtin_memory_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory &runtime_memory);
    void register_task_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_inbox_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_message_attachments_tool(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context);
    void register_skill_tool(ToolRegistry &registry, const skills::SkillLoader &skill_loader);
    void register_tool_search(ToolRegistry &registry);

} // namespace orangutan::tools
