#pragma once

#include "core/tools/permissions.hpp"
#include "core/tools/tool.hpp"

#include <string>
#include <string_view>

namespace orangutan {

    void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace = {}, const ToolRuntimeContext *tool_context = nullptr,
                                     const ToolPermissionSettings *permissions = nullptr, std::string_view edit_mode = "search_replace");
    void register_builtin_subagent_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_builtin_memory_tools(ToolRegistry &registry, RuntimeMemory &runtime_memory);
    void register_task_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_inbox_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan
