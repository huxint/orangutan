#pragma once

#include "permissions/permission-types.hpp"
#include "tools/file/edit/edit-mode.hpp"
#include "tools/registry/tool-registry.hpp"

#include <filesystem>
#include <string>

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::tools {

    void register_builtin_core_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root = {}, const ToolRuntimeContext *tool_context = nullptr,
                                     const ToolPermissionContext *permissions = nullptr, file::edit_mode mode = file::DEFAULT_EDIT_MODE);
    void register_builtin_memory_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory &runtime_memory);
    void register_automation_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_message_attachments_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context);
    void register_skill_tool(ToolRegistry &registry, const skills::SkillLoader &skill_loader);
    void register_tool_search(ToolRegistry &registry);

} // namespace orangutan::tools
