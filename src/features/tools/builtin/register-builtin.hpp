#pragma once

#include "core/tools/tool.hpp"

#include <string>

namespace orangutan {

void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace = {});
void register_builtin_subagent_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
void register_builtin_memory_tools(ToolRegistry &registry, RuntimeMemory &runtime_memory);
void register_cron_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);

} // namespace orangutan
