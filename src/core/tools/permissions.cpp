#include "core/tools/permissions.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace orangutan {
namespace {

std::string normalize_token(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '-' || ch == '_') {
            normalized.push_back('-');
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

std::string lowercase_copy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const auto ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::optional<std::string> extract_shell_command(const ToolUseBlock &call) {
    if (call.name != "shell" || !call.input.contains("command") || !call.input["command"].is_string()) {
        return std::nullopt;
    }
    return call.input.at("command").get<std::string>();
}

ToolResultBlock blocked_result(const ToolUseBlock &call, std::string message) {
    return ToolResultBlock{
        .tool_use_id = call.id,
        .content = std::move(message),
        .is_error = true,
    };
}

} // namespace

std::optional<ToolSandboxMode> parse_tool_sandbox_mode(std::string_view value) {
    const auto normalized = normalize_token(value);
    if (normalized == "isolated") {
        return ToolSandboxMode::isolated;
    }
    if (normalized == "workspace-write") {
        return ToolSandboxMode::workspace_write;
    }
    if (normalized == "disabled") {
        return ToolSandboxMode::disabled;
    }
    return std::nullopt;
}

std::optional<ToolApprovalPolicy> parse_tool_approval_policy(std::string_view value) {
    const auto normalized = normalize_token(value);
    if (normalized == "ask") {
        return ToolApprovalPolicy::ask;
    }
    if (normalized == "allow") {
        return ToolApprovalPolicy::allow;
    }
    if (normalized == "deny") {
        return ToolApprovalPolicy::deny;
    }
    return std::nullopt;
}

std::string to_string(ToolSandboxMode mode) {
    switch (mode) {
        case ToolSandboxMode::isolated:
            return "isolated";
        case ToolSandboxMode::workspace_write:
            return "workspace-write";
        case ToolSandboxMode::disabled:
            return "disabled";
    }
    return "disabled";
}

std::string to_string(ToolApprovalPolicy policy) {
    switch (policy) {
        case ToolApprovalPolicy::ask:
            return "ask";
        case ToolApprovalPolicy::allow:
            return "allow";
        case ToolApprovalPolicy::deny:
            return "deny";
    }
    return "deny";
}

bool is_tool_allowed(const ToolPermissionSettings &settings, std::string_view name) {
    const auto denied = std::ranges::find(settings.denied_tools, name);
    if (denied != settings.denied_tools.end()) {
        return false;
    }
    if (settings.allowed_tools.empty()) {
        return true;
    }
    return std::ranges::find(settings.allowed_tools, name) != settings.allowed_tools.end();
}

std::optional<std::string> blocked_shell_command_pattern(const ToolPermissionSettings &settings, std::string_view command) {
    const auto lowered_command = lowercase_copy(command);
    for (const auto &pattern : settings.denied_shell_commands) {
        const auto lowered_pattern = lowercase_copy(pattern);
        if (!lowered_pattern.empty() && lowered_command.find(lowered_pattern) != std::string::npos) {
            return pattern;
        }
    }
    return std::nullopt;
}

std::optional<ToolResultBlock> evaluate_tool_permission(const ToolUseBlock &call, const ToolPermissionSettings &settings, const ToolApprovalCallback &approval_callback) {
    if (!is_tool_allowed(settings, call.name)) {
        return blocked_result(call, "Tool '" + call.name + "' blocked by permission policy.");
    }

    const auto maybe_command = extract_shell_command(call);
    if (!maybe_command.has_value()) {
        return std::nullopt;
    }

    return evaluate_shell_command_permission(call, settings, *maybe_command, approval_callback);
}

std::optional<ToolResultBlock> evaluate_shell_command_permission(const ToolUseBlock &call, const ToolPermissionSettings &settings, std::string_view command,
                                                                 const ToolApprovalCallback &approval_callback) {
    if (!is_tool_allowed(settings, call.name)) {
        return blocked_result(call, "Tool '" + call.name + "' blocked by permission policy.");
    }

    if (auto blocked_pattern = blocked_shell_command_pattern(settings, command); blocked_pattern.has_value()) {
        return blocked_result(call, "Shell command blocked by permission policy: matched '" + *blocked_pattern + "'.");
    }

    switch (settings.shell_approval) {
        case ToolApprovalPolicy::allow:
            return std::nullopt;
        case ToolApprovalPolicy::deny:
            return blocked_result(call, "Shell tool blocked by approval policy.");
        case ToolApprovalPolicy::ask:
            break;
    }

    if (!approval_callback) {
        return blocked_result(call, "Shell command requires approval, but interactive approval is unavailable.");
    }

    std::ostringstream prompt;
    prompt << "Shell command approval required.\n";
    prompt << "Tool: " << call.name << '\n';
    prompt << "Sandbox mode: " << to_string(settings.sandbox_mode) << '\n';
    prompt << "Command: " << command;
    if (!approval_callback(call, prompt.str())) {
        return blocked_result(call, "Shell command rejected by user.");
    }

    return std::nullopt;
}

} // namespace orangutan
