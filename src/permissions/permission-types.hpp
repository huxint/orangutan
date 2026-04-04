#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace orangutan {
    struct ToolUse;
}

namespace orangutan::permissions {

    // ── Permission Mode ──────────────────────────────────────────────────

    enum class PermissionMode {
        default_mode,
        accept_edits,
        plan,
        bypass_permissions,
        dont_ask,
    };

    // ── Permission Behavior ──────────────────────────────────────────────

    enum class PermissionBehavior {
        allow,
        deny,
        ask,
    };

    // A tool's check_permissions() can return passthrough to defer to the pipeline.
    struct PermissionResult {
        PermissionBehavior behavior = PermissionBehavior::ask;
        bool is_passthrough = false;
        std::optional<std::string> message;

        static PermissionResult passthrough() { return {.behavior = PermissionBehavior::ask, .is_passthrough = true}; }
        static PermissionResult allow() { return {.behavior = PermissionBehavior::allow}; }
        static PermissionResult deny(std::string msg) { return {.behavior = PermissionBehavior::deny, .is_passthrough = false, .message = std::move(msg)}; }
        static PermissionResult ask(std::string msg) { return {.behavior = PermissionBehavior::ask, .is_passthrough = false, .message = std::move(msg)}; }
    };

    // ── Permission Rules ─────────────────────────────────────────────────

    enum class RuleMatchType {
        exact,
        prefix,
        wildcard,
    };

    struct RuleContent {
        RuleMatchType match_type = RuleMatchType::exact;
        std::string pattern;
    };

    enum class PermissionRuleSource {
        cli_arg,
        session,
        local_settings,
        project_settings,
        user_settings,
    };

    struct PermissionRule {
        PermissionRuleSource source;
        PermissionBehavior behavior;
        std::string tool_name;
        std::optional<RuleContent> content; // nullopt = whole-tool rule
    };

    // ── Decision Reason ──────────────────────────────────────────────────

    struct RuleDecisionReason {
        PermissionRuleSource source;
        std::string rule_value;
    };

    struct ModeDecisionReason {
        PermissionMode mode;
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
        PermissionBehavior behavior = PermissionBehavior::ask;
        std::optional<std::string> message;
        std::optional<DecisionReason> reason;

        static PermissionDecision allow_by_rule(PermissionRuleSource source, std::string rule_value) {
            return {.behavior = PermissionBehavior::allow, .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision deny_by_rule(PermissionRuleSource source, std::string rule_value, std::string msg) {
            return {.behavior = PermissionBehavior::deny, .message = std::move(msg), .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision ask_by_rule(PermissionRuleSource source, std::string rule_value, std::string msg) {
            return {.behavior = PermissionBehavior::ask, .message = std::move(msg), .reason = RuleDecisionReason{.source = source, .rule_value = std::move(rule_value)}};
        }
        static PermissionDecision allow_by_mode(PermissionMode mode) {
            return {.behavior = PermissionBehavior::allow, .reason = ModeDecisionReason{.mode = mode}};
        }
        static PermissionDecision deny_by_mode(PermissionMode mode, std::string msg) {
            return {.behavior = PermissionBehavior::deny, .message = std::move(msg), .reason = ModeDecisionReason{.mode = mode}};
        }
        static PermissionDecision ask_by_safety(std::string path, std::string msg) {
            return {.behavior = PermissionBehavior::ask, .message = std::move(msg), .reason = SafetyCheckDecisionReason{.path = std::move(path)}};
        }
        static PermissionDecision deny_by_hook() {
            return {.behavior = PermissionBehavior::deny, .message = "Tool call blocked by hook", .reason = HookDecisionReason{}};
        }
        static PermissionDecision ask_default(std::string msg) {
            return {.behavior = PermissionBehavior::ask, .message = std::move(msg)};
        }
    };

    // ── Tool Permission Context ──────────────────────────────────────────

    struct ToolPermissionContext {
        PermissionMode mode = PermissionMode::default_mode;
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
    using permissions::PermissionBehavior;
    using permissions::PermissionDecision;
    using permissions::PermissionMode;
    using permissions::PermissionResult;
    using permissions::PermissionRule;
    using permissions::PermissionRuleSource;
    using permissions::RuleContent;
    using permissions::RuleDecisionReason;
    using permissions::RuleMatchType;
    using permissions::SafetyCheckDecisionReason;
    using permissions::ToolPermissionContext;
    using permissions::ToolSpecificDecisionReason;

} // namespace orangutan
