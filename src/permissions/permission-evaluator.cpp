#include "permissions/approval-signature.hpp"
#include "permissions/permission-evaluator.hpp"

#include "permissions/permission-display.hpp"
#include "permissions/rule-parser.hpp"
#include "permissions/safety-checks.hpp"
#include "types/content.hpp"

#include <string>

namespace orangutan::permissions {

    static bool is_shell_tool_name(std::string_view name) {
        return name == "shell";
    }

    bool is_file_tool_name(std::string_view name) {
        return name == "read" || name == "write" || name == "edit" || name.contains("file_read") || name.contains("file_write") || name.contains("file_edit");
    }

    static bool is_mutating_file_tool_name(std::string_view name) {
        return name == "write" || name == "edit" || name.contains("file_write") || name.contains("file_edit");
    }

    static bool requires_default_approval(std::string_view name) {
        return is_shell_tool_name(name) || is_mutating_file_tool_name(name);
    }

    static std::string extract_tool_content(const ToolUse &call) {
        return approval_match_content(call);
    }

    std::optional<PermissionRule> find_matching_rule(std::string_view tool_name, std::string_view content, const std::vector<PermissionRule> &rules) {
        for (const auto &rule : rules) {
            if (matches_rule(rule, tool_name, content)) {
                return rule;
            }
        }
        return std::nullopt;
    }

    static std::string rule_value_string(const PermissionRule &rule) {
        auto value = rule.tool_name;
        if (rule.content.has_value()) {
            value += "(" + rule.content->pattern + ")";
        }
        return value;
    }

    static std::optional<PermissionDecision> make_rule_decision(std::string_view tool_name, std::string_view content, const std::vector<PermissionRule> &rules,
                                                                permission_behavior behavior) {
        const auto rule = find_matching_rule(tool_name, content, rules);
        if (!rule.has_value()) {
            return std::nullopt;
        }

        auto value = rule_value_string(*rule);
        switch (behavior) {
            case permission_behavior::deny:
                return PermissionDecision::deny_by_rule(rule->source, std::move(value), "Blocked by deny rule");
            case permission_behavior::ask:
                return PermissionDecision::ask_by_rule(rule->source, std::move(value), "Requires approval per ask rule");
            case permission_behavior::allow:
                return PermissionDecision::allow_by_rule(rule->source, std::move(value));
        }
        return std::nullopt;
    }

    std::optional<PermissionDecision> evaluate_mode(permission_mode mode, const ToolUse &call, bool is_read_only, bool is_file_tool_in_workspace) {
        switch (mode) {
            case permission_mode::bypass_permissions:
                return PermissionDecision::allow_by_mode(mode);

            case permission_mode::accept_edits:
                if (is_file_tool_name(call.name) && is_file_tool_in_workspace) {
                    return PermissionDecision::allow_by_mode(mode);
                }
                if (is_read_only) {
                    return PermissionDecision::allow_by_mode(mode);
                }
                return std::nullopt;

            case permission_mode::plan:
                if (is_file_tool_name(call.name) && is_file_tool_in_workspace) {
                    return PermissionDecision::allow_by_mode(mode);
                }
                if (is_read_only) {
                    return PermissionDecision::allow_by_mode(mode);
                }
                return PermissionDecision::deny_by_mode(mode, "Plan mode: write operations not allowed");

            case permission_mode::dont_ask:
            case permission_mode::default_mode:
                return std::nullopt;
        }
        return std::nullopt;
    }

    PermissionDecision apply_post_processing(PermissionDecision decision, permission_mode mode) {
        if (mode == permission_mode::dont_ask && decision.behavior == permission_behavior::ask) {
            return PermissionDecision::deny_by_mode(mode, decision.message.value_or("Denied: dont_ask mode"));
        }
        return decision;
    }

    PermissionDecision evaluate_permission(const ToolUse &call, const ToolPermissionContext &ctx, const ToolPermissionChecker &tool_checker,
                                           const IsReadOnlyChecker &is_read_only) {
        if (ctx.mode == permission_mode::bypass_permissions) {
            return PermissionDecision::allow_by_mode(ctx.mode);
        }

        auto content = extract_tool_content(call);
        bool is_file_tool_in_workspace = false;

        if (auto decision = make_rule_decision(call.name, content, ctx.deny_rules, permission_behavior::deny); decision.has_value()) {
            return *decision;
        }

        if (auto decision = make_rule_decision(call.name, content, ctx.ask_rules, permission_behavior::ask); decision.has_value()) {
            return *decision;
        }

        if (tool_checker) {
            auto result = tool_checker(call, ctx);
            if (!result.is_passthrough) {
                auto detail = result.message.value_or("");
                return {.behavior = result.behavior, .message = std::move(result.message), .reason = ToolSpecificDecisionReason{.detail = std::move(detail)}};
            }
            if (is_file_tool_name(call.name)) {
                is_file_tool_in_workspace = true;
            }
        }

        if (auto safety = check_safety(call)) {
            return *safety;
        }

        bool is_read_only_val = is_read_only ? is_read_only() : false;
        if (auto mode_decision = evaluate_mode(ctx.mode, call, is_read_only_val, is_file_tool_in_workspace)) {
            return *mode_decision;
        }

        if (auto decision = make_rule_decision(call.name, content, ctx.allow_rules, permission_behavior::allow); decision.has_value()) {
            return *decision;
        }

        if (!requires_default_approval(call.name)) {
            return PermissionDecision::allow_by_mode(ctx.mode);
        }

        return PermissionDecision::ask_default(default_tool_approval_message(call.name));
    }

} // namespace orangutan::permissions
