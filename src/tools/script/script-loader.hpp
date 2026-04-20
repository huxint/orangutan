#pragma once

#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::tools {

    using ScriptToolConfig = Config::ScriptToolConfig;

    std::string substitute_params(const std::string &command_template, const nlohmann::json &input, const std::unordered_map<std::string, std::string> &schema);

    nlohmann::json generate_input_schema(const std::unordered_map<std::string, std::string> &schema);

    void register_script_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::filesystem::path &workspace_root = {},
                               const ToolPermissionContext *permissions = nullptr, const ToolRuntimeContext *tool_context = nullptr,
                               const ApprovalCallback &approval_callback = {});

} // namespace orangutan::tools
