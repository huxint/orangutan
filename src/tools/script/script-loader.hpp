#pragma once

#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "tools/registry/tool.hpp"
#include "utils/escape.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::tools {

    using ScriptToolConfig = Config::ScriptToolConfig;

    inline auto &shell_escape = utils::shell_single_quote_escape;

    // Substitute ${param} patterns in a command template with shell-escaped values from input JSON.
    std::string substitute_params(const std::string &command_template, const nlohmann::json &input, const std::unordered_map<std::string, std::string> &schema);

    // Generate a JSON Schema object from a flat param_name → type_string map.
    nlohmann::json generate_input_schema(const std::unordered_map<std::string, std::string> &schema);

    // Register user-defined script tools from config into the registry.
    void register_script_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::filesystem::path &workspace_root = {},
                               const ToolPermissionContext *permissions = nullptr, const ToolRuntimeContext *tool_context = nullptr,
                               const ApprovalCallback &approval_callback = {});

} // namespace orangutan::tools
