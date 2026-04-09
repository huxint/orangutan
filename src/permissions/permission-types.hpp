#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "types/base.hpp"

namespace orangutan {
    struct ToolUse;
}

namespace orangutan::permissions {

    // ── Permission Mode ──────────────────────────────────────────────────

    enum class permission_mode : base::u8 {
        default_mode,
        accept_edits,
        plan,
        bypass_permissions,
        dont_ask,
    };

    // ── Permission Behavior ──────────────────────────────────────────────

    enum class permission_behavior : base::u8 {
        allow,
        deny,
        ask,
    };

    // A tool's check_permissions() can return passthrough to defer to the pipeline.
    struct PermissionResult {
        permission_behavior behavior = permission_behavior::ask;
        bool is_passthrough = false;
        std::optional<std::string> message;

        static PermissionResult passthrough() {
            return {.behavior = permission_behavior::ask, .is_passthrough = true};
        }
        static PermissionResult allow() {
            return {.behavior = permission_behavior::allow};
        }
        static PermissionResult deny(std::string msg) {
            return {.behavior = permission_behavior::deny, .is_passthrough = false, .message = std::move(msg)};
        }
        static PermissionResult ask(std::string msg) {
            return {.behavior = permission_behavior::ask, .is_passthrough = false, .message = std::move(msg)};
        }
    };

    // ── Permission Rules ─────────────────────────────────────────────────

    enum class rule_match_type : base::u8 {
        exact,
        prefix,
        wildcard,
    };

    struct RuleContent {
        rule_match_type match_type = rule_match_type::exact;
        std::string pattern;
    };

    enum class permission_rule_source : base::u8 {
        cli_arg,
        session,
        local_settings,
        project_settings,
        user_settings,
    };

    struct PermissionRule {
        permission_rule_source source;
        permission_behavior behavior;
        std::string tool_name;
        std::optional<RuleContent> content; // nullopt = whole-tool rule
    };

    // ── Decision Reason ──────────────────────────────────────────────────

    struct RuleDecisionReason {
        permission_rule_source source;
        std::string rule_value;
    };

    struct ModeDecisionReason {
        permission_mode mode;
    };

    struct SafetyCheckDecisionReason {
        std::string path;
    };

    struct ToolSpecificDecisionReason {
        std::string detail;
    };

    struct HookDecisionReason {};

    using DecisionReason = std::variant<RuleDecisionReason, ModeDecisionReason, SafetyCheckDecisionReason, ToolSpecificDecisionReason, HookDecisionReason>;

    struct PermissionDecision {
        permission_behavior behavior = permission_behavior::ask;
        std::optional<std::string> message;
        std::optional<DecisionReason> reason;

        static PermissionDecision allow_by_rule(permission_rule_source source, std::string rule_value) {
            return {.behavior = permission_behavior::allow, .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision deny_by_rule(permission_rule_source source, std::string rule_value, std::string msg) {
            return {.behavior = permission_behavior::deny, .message = std::move(msg), .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision ask_by_rule(permission_rule_source source, std::string rule_value, std::string msg) {
            return {.behavior = permission_behavior::ask, .message = std::move(msg), .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision allow_by_mode(permission_mode mode) {
            return {.behavior = permission_behavior::allow, .reason = ModeDecisionReason{.mode = mode}};
        }
        static PermissionDecision deny_by_mode(permission_mode mode, std::string msg) {
            return {.behavior = permission_behavior::deny, .message = std::move(msg), .reason = ModeDecisionReason{.mode = mode}};
        }
        static PermissionDecision ask_by_safety(std::string path, std::string msg) {
            return {.behavior = permission_behavior::ask, .message = std::move(msg), .reason = SafetyCheckDecisionReason{.path = std::move(path)}};
        }
        static PermissionDecision deny_by_hook() {
            return {.behavior = permission_behavior::deny, .message = "Tool call blocked by hook", .reason = HookDecisionReason{}};
        }
        static PermissionDecision ask_default(std::string msg) {
            return {.behavior = permission_behavior::ask, .message = std::move(msg)};
        }
    };

    // ── Tool Permission Context ──────────────────────────────────────────

    struct ToolPermissionContext {
        permission_mode mode = permission_mode::default_mode;
        std::vector<PermissionRule> allow_rules;
        std::vector<PermissionRule> deny_rules;
        std::vector<PermissionRule> ask_rules;
        bool is_bypass_available = false;
        std::vector<std::string> additional_directories;
    };

    // ── Approval Callback ────────────────────────────────────────────────

    // Returns true if the user approves the tool call. The PermissionDecision
    // carries the reason/message to display to the user.
    using ApprovalCallback = std::function<bool(const ToolUse &call, const PermissionDecision &decision)>;

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::ApprovalCallback;
    using permissions::DecisionReason;
    using permissions::HookDecisionReason;
    using permissions::ModeDecisionReason;
    using permissions::permission_behavior;
    using permissions::permission_mode;
    using permissions::permission_rule_source;
    using permissions::PermissionDecision;
    using permissions::PermissionResult;
    using permissions::PermissionRule;
    using permissions::rule_match_type;
    using permissions::RuleContent;
    using permissions::RuleDecisionReason;
    using permissions::SafetyCheckDecisionReason;
    using permissions::ToolPermissionContext;
    using permissions::ToolSpecificDecisionReason;

} // namespace orangutan
