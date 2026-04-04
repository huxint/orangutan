#include "permissions/permission-evaluator.hpp"

#include "permissions/rule-parser.hpp"
#include "permissions/safety-checks.hpp"
#include "types/content.hpp"

#include <string>

namespace orangutan::permissions {

    static bool is_shell_tool(std::string_view name) {
        return name == "shell";
    }

    static bool is_file_tool(std::string_view name) {
        return name == "read"
            || name == "write"
            || name == "edit"
            || name.contains("file_read")
            || name.contains("file_write")
            || name.contains("file_edit");
    }

    static std::string extract_tool_content(const ToolUse &call) {
        if (is_shell_tool(call.name)) {
            if (call.input.contains("command") && call.input["command"].is_string()) {
                return call.input["command"].get<std::string>();
            }
        }
        if (is_file_tool(call.name)) {
            if (call.input.contains("path") && call.input["path"].is_string()) {
                return call.input["path"].get<std::string>();
            }
            if (call.input.contains("file_path") && call.input["file_path"].is_string()) {
                return call.input["file_path"].get<std::string>();
            }
        }
        return {};
    }

    std::optional<PermissionRule> find_matching_rule(std::string_view tool_name, std::string_view content,
                                                     const std::vector<PermissionRule> &rules) {
        for (const auto &rule : rules) {
            if (matches_rule(rule, tool_name, content)) {
                return rule;
            }
        }
        return std::nullopt;
    }

    std::optional<PermissionDecision> evaluate_mode(PermissionMode mode, const ToolUse &call,
                                                     bool is_read_only, bool is_file_tool_in_workspace) {
        switch (mode) {
        case PermissionMode::bypass_permissions:
            return PermissionDecision::allow_by_mode(mode);

        case PermissionMode::accept_edits:
            if (is_file_tool(call.name) && is_file_tool_in_workspace) {
                return PermissionDecision::allow_by_mode(mode);
            }
            if (is_read_only) {
                return PermissionDecision::allow_by_mode(mode);
            }
            return std::nullopt;

        case PermissionMode::plan:
            if (is_read_only) {
                return PermissionDecision::allow_by_mode(mode);
            }
            return PermissionDecision::deny_by_mode(mode, "Plan mode: write operations not allowed");

        case PermissionMode::dont_ask:
        case PermissionMode::default_mode:
            return std::nullopt;
        }
        return std::nullopt;
    }

    PermissionDecision apply_post_processing(PermissionDecision decision, PermissionMode mode) {
        if (mode == PermissionMode::dont_ask && decision.behavior == PermissionBehavior::ask) {
            return PermissionDecision::deny_by_mode(mode, decision.message.value_or("Denied: dont_ask mode"));
        }
        return decision;
    }

    PermissionDecision evaluate_permission(const ToolUse &call, const ToolPermissionContext &ctx,
                                           const ToolPermissionChecker &tool_checker,
                                           const IsReadOnlyChecker &is_read_only) {
        auto content = extract_tool_content(call);

        if (auto rule = find_matching_rule(call.name, content, ctx.deny_rules)) {
            auto rule_value = rule->tool_name;
            if (rule->content) {
                rule_value += "(" + rule->content->pattern + ")";
            }
            return PermissionDecision::deny_by_rule(rule->source, std::move(rule_value), "Blocked by deny rule");
        }

        if (auto rule = find_matching_rule(call.name, content, ctx.ask_rules)) {
            auto rule_value = rule->tool_name;
            if (rule->content) {
                rule_value += "(" + rule->content->pattern + ")";
            }
            return PermissionDecision::ask_by_rule(rule->source, std::move(rule_value), "Requires approval per ask rule");
        }

        if (tool_checker) {
            auto result = tool_checker(call, ctx);
            if (!result.is_passthrough) {
                auto detail = result.message.value_or("");
                return {.behavior = result.behavior,
                        .message = std::move(result.message),
                        .reason = ToolSpecificDecisionReason{.detail = std::move(detail)}};
            }
        }

        if (auto safety = check_safety(call)) {
            return *safety;
        }

        bool is_read_only_val = is_read_only ? is_read_only() : false;
        if (auto mode_decision = evaluate_mode(ctx.mode, call, is_read_only_val)) {
            return *mode_decision;
        }

        if (auto rule = find_matching_rule(call.name, content, ctx.allow_rules)) {
            auto rule_value = rule->tool_name;
            if (rule->content) {
                rule_value += "(" + rule->content->pattern + ")";
            }
            return PermissionDecision::allow_by_rule(rule->source, std::move(rule_value));
        }

        return PermissionDecision::ask_default("Tool '" + call.name + "' requires approval");
    }

} // namespace orangutan::permissions
