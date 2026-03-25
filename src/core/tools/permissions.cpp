#include "core/tools/permissions.hpp"

#include <algorithm>
#include <cctype>
#include "infra/format.hpp"

#include <magic_enum/magic_enum.hpp>

namespace orangutan {
namespace {

std::string normalize_enum_token(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '-' || ch == '_') {
            normalized.push_back('_');
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

template <typename Enum>
std::string hyphenated_enum_name_or(Enum value, std::string_view fallback) {
    const auto name = magic_enum::enum_name(value);
    std::string rendered = name.empty() ? std::string(fallback) : std::string(name);
    std::ranges::replace(rendered, '_', '-');
    return rendered;
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
    return magic_enum::enum_cast<ToolSandboxMode>(normalize_enum_token(value));
}

std::optional<ToolApprovalPolicy> parse_tool_approval_policy(std::string_view value) {
    return magic_enum::enum_cast<ToolApprovalPolicy>(normalize_enum_token(value));
}

std::string to_string(ToolSandboxMode mode) {
    return hyphenated_enum_name_or(mode, "disabled");
}

std::string to_string(ToolApprovalPolicy policy) {
    return hyphenated_enum_name_or(policy, "deny");
}

bool is_tool_allowed(const ToolPermissionSettings &settings, std::string_view name) {
    if (std::ranges::contains(settings.denied_tools, name)) {
        return false;
    }
    if (settings.allowed_tools.empty()) {
        return true;
    }
    return std::ranges::contains(settings.allowed_tools, name);
}

std::optional<std::string> blocked_shell_command_pattern(const ToolPermissionSettings &settings, std::string_view command) {
    const auto lowered_command = lowercase_copy(command);
    for (const auto &pattern : settings.denied_shell_commands) {
        const auto lowered_pattern = lowercase_copy(pattern);
        if (!lowered_pattern.empty() && lowered_command.contains(lowered_pattern)) {
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

    if (approval_callback == nullptr) {
        return blocked_result(call, "Shell command requires approval, but interactive approval is unavailable.");
    }

    std::string prompt;
    prompt += "Shell command approval required.\n";
    append(prompt, "Tool: {}\n", call.name);
    append(prompt, "Sandbox mode: {}\n", to_string(settings.sandbox_mode));
    append(prompt, "Command: {}", command);
    if (!approval_callback(call, prompt)) {
        return blocked_result(call, "Shell command rejected by user.");
    }

    return std::nullopt;
}

} // namespace orangutan
