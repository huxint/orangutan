#pragma once

#include "core/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan {

enum class ToolSandboxMode {
    isolated,
    workspace_write,
    disabled,
};

enum class ToolApprovalPolicy {
    ask,
    allow,
    deny,
};

struct ToolPermissionSettings {
    ToolSandboxMode sandbox_mode = ToolSandboxMode::disabled;
    ToolApprovalPolicy shell_approval = ToolApprovalPolicy::allow;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;
    std::vector<std::string> denied_shell_commands;
};

using ToolApprovalCallback = std::function<bool(const ToolUseBlock &call, const std::string &prompt_text)>;

[[nodiscard]]
std::optional<ToolSandboxMode> parse_tool_sandbox_mode(std::string_view value);

[[nodiscard]]
std::optional<ToolApprovalPolicy> parse_tool_approval_policy(std::string_view value);

[[nodiscard]]
std::string to_string(ToolSandboxMode mode);

[[nodiscard]]
std::string to_string(ToolApprovalPolicy policy);

[[nodiscard]]
bool is_tool_allowed(const ToolPermissionSettings &settings, std::string_view name);

[[nodiscard]]
std::optional<std::string> blocked_shell_command_pattern(const ToolPermissionSettings &settings, std::string_view command);

[[nodiscard]]
std::optional<ToolResultBlock> evaluate_tool_permission(const ToolUseBlock &call, const ToolPermissionSettings &settings,
                                                        const ToolApprovalCallback &approval_callback = {});

} // namespace orangutan
