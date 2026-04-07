#pragma once

#include "permissions/permission-types.hpp"

#include <nlohmann/json.hpp>

#include <magic_enum/magic_enum.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace orangutan::permissions {

    inline std::string permission_behavior_label(permission_behavior behavior) {
        return std::string{magic_enum::enum_name(behavior)};
    }

    inline std::string permission_mode_label(permission_mode mode) {
        return std::string{magic_enum::enum_name(mode)};
    }

    inline std::string permission_rule_source_label(permission_rule_source source) {
        switch (source) {
            case permission_rule_source::cli_arg:
                return "CLI";
            case permission_rule_source::session:
                return "session";
            case permission_rule_source::local_settings:
                return "local settings";
            case permission_rule_source::project_settings:
                return "project settings";
            case permission_rule_source::user_settings:
                return "user settings";
        }
        return "unknown";
    }

    inline std::string default_tool_approval_message(std::string_view tool_name) {
        return "Tool '" + std::string(tool_name) + "' requires approval";
    }

    inline std::string approval_prompt_message(const PermissionDecision &decision, std::string_view fallback = "Tool requires approval") {
        return decision.message.value_or(std::string{fallback});
    }

    inline nlohmann::json permission_reason_to_json(const DecisionReason &reason) {
        return std::visit(
            [](const auto &value) -> nlohmann::json {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, RuleDecisionReason>) {
                    return {
                        {"type", "rule"},
                        {"source", std::string{magic_enum::enum_name(value.source)}},
                        {"source_label", permission_rule_source_label(value.source)},
                        {"rule_value", value.rule_value},
                    };
                } else if constexpr (std::is_same_v<Value, ModeDecisionReason>) {
                    return {
                        {"type", "mode"},
                        {"mode", std::string{magic_enum::enum_name(value.mode)}},
                        {"mode_label", permission_mode_label(value.mode)},
                    };
                } else if constexpr (std::is_same_v<Value, SafetyCheckDecisionReason>) {
                    return {
                        {"type", "safety_check"},
                        {"path", value.path},
                    };
                } else if constexpr (std::is_same_v<Value, ToolSpecificDecisionReason>) {
                    return {
                        {"type", "tool_specific"},
                        {"detail", value.detail},
                    };
                } else {
                    return {
                        {"type", "hook"},
                    };
                }
            },
            reason);
    }

    inline nlohmann::json permission_decision_to_json(const PermissionDecision &decision) {
        nlohmann::json payload = {
            {"behavior", permission_behavior_label(decision.behavior)},
        };
        if (decision.message.has_value()) {
            payload["message"] = *decision.message;
        }
        if (decision.reason.has_value()) {
            payload["reason"] = permission_reason_to_json(*decision.reason);
        }
        return payload;
    }

    inline std::vector<std::string> permission_decision_detail_lines(const PermissionDecision &decision) {
        std::vector<std::string> lines;
        lines.push_back("Behavior: " + permission_behavior_label(decision.behavior));
        if (!decision.reason.has_value()) {
            return lines;
        }

        std::visit(
            [&lines](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, RuleDecisionReason>) {
                    lines.push_back("Reason: rule from " + permission_rule_source_label(value.source));
                    lines.push_back("Rule: " + value.rule_value);
                } else if constexpr (std::is_same_v<Value, ModeDecisionReason>) {
                    lines.push_back("Reason: mode");
                    lines.push_back("Mode: " + permission_mode_label(value.mode));
                } else if constexpr (std::is_same_v<Value, SafetyCheckDecisionReason>) {
                    lines.push_back("Reason: safety check");
                    lines.push_back("Path: " + value.path);
                } else if constexpr (std::is_same_v<Value, ToolSpecificDecisionReason>) {
                    lines.push_back("Reason: tool-specific check");
                    lines.push_back("Detail: " + value.detail);
                } else {
                    lines.push_back("Reason: hook");
                }
            },
            *decision.reason);
        return lines;
    }

} // namespace orangutan::permissions
