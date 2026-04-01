#pragma once

#include "tools/registry/permissions.hpp"
#include "infra/config/config.hpp"
#include "tools/registry/tool.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan {

    using ScriptToolConfig = Config::ScriptToolConfig;

    // Shell-escape a value for safe substitution into a command string.
    // Wraps in single quotes and escapes embedded single quotes.
    std::string shell_escape(const std::string &value);

    // Substitute ${param} patterns in a command template with shell-escaped values from input JSON.
    std::string substitute_params(const std::string &command_template, const nlohmann::json &input, const std::unordered_map<std::string, std::string> &schema);

    // Generate a JSON Schema object from a flat param_name → type_string map.
    nlohmann::json generate_input_schema(const std::unordered_map<std::string, std::string> &schema);

    // Register user-defined script tools from config into the registry.
    void register_script_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace = {},
                               const ToolPermissionSettings *permissions = nullptr, const ToolRuntimeContext *tool_context = nullptr,
                               const ToolApprovalCallback &approval_callback = {});

} // namespace orangutan
