#include "permissions/approval-signature.hpp"
#include "permissions/rule-parser.hpp"
#include "permissions/permission-display.hpp"
#include "permissions/permission-evaluator.hpp"
#include "permissions/safety-checks.hpp"
#include "permissions/permission-state.hpp"
#include "types/content.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

using namespace orangutan;
using namespace orangutan::permissions;

// ── Rule Parsing ─────────────────────────────────────────────────────────

TEST_CASE("ParseWholeToolRule") {
    auto rule = parse_permission_rule("Read", permission_behavior::allow, permission_rule_source::user_settings);
    REQUIRE(rule.tool_name == "Read");
    REQUIRE(rule.behavior == permission_behavior::allow);
    REQUIRE(rule.source == permission_rule_source::user_settings);
    REQUIRE(!rule.content.has_value());
}

TEST_CASE("ParseExactContentRule") {
    auto rule = parse_permission_rule("Shell(npm install)", permission_behavior::deny, permission_rule_source::cli_arg);
    REQUIRE(rule.tool_name == "Shell");
    REQUIRE(rule.content.has_value());
    REQUIRE(rule.content->match_type == rule_match_type::exact);
    REQUIRE(rule.content->pattern == "npm install");
}

TEST_CASE("ParsePrefixRule") {
    auto rule = parse_permission_rule("Shell(npm:*)", permission_behavior::allow, permission_rule_source::session);
    REQUIRE(rule.tool_name == "Shell");
    REQUIRE(rule.content.has_value());
    REQUIRE(rule.content->match_type == rule_match_type::prefix);
    REQUIRE(rule.content->pattern == "npm");
}

TEST_CASE("ParseWildcardRule") {
    auto rule = parse_permission_rule("Shell(git * --force)", permission_behavior::deny, permission_rule_source::project_settings);
    REQUIRE(rule.tool_name == "Shell");
    REQUIRE(rule.content.has_value());
    REQUIRE(rule.content->match_type == rule_match_type::wildcard);
    REQUIRE(rule.content->pattern == "git * --force");
}

// ── Prefix Matching ──────────────────────────────────────────────────────

TEST_CASE("PrefixMatchSuccess") {
    REQUIRE(matches_prefix("npm", "npm install express"));
    REQUIRE(matches_prefix("npm", "npm"));
}

TEST_CASE("PrefixMatchFailsWithoutWordBoundary") {
    REQUIRE_FALSE(matches_prefix("npm", "npmx something"));
}

TEST_CASE("PrefixMatchFailsOnShorterInput") {
    REQUIRE_FALSE(matches_prefix("npm install", "npm"));
}

// ── Wildcard Matching ────────────────────────────────────────────────────

TEST_CASE("WildcardMatchSuccess") {
    REQUIRE(matches_wildcard("git * --force", "git push --force"));
    REQUIRE(matches_wildcard("rm *", "rm -rf /tmp"));
}

TEST_CASE("WildcardMatchFails") {
    REQUIRE_FALSE(matches_wildcard("git * --force", "git push --no-verify"));
}

// ── Rule Matching ────────────────────────────────────────────────────────

TEST_CASE("WholeToolRuleMatchesAnyContent") {
    auto rule = parse_permission_rule("shell", permission_behavior::allow, permission_rule_source::user_settings);
    REQUIRE(matches_rule(rule, "shell", "ls -la"));
    REQUIRE(matches_rule(rule, "shell", "rm -rf /"));
    REQUIRE(matches_rule(rule, "shell"));
}

TEST_CASE("RuleMatchIsCaseInsensitive") {
    auto rule = parse_permission_rule("Shell", permission_behavior::allow, permission_rule_source::user_settings);
    REQUIRE(matches_rule(rule, "shell", "ls"));
    REQUIRE(matches_rule(rule, "SHELL", "ls"));
}

TEST_CASE("ContentRuleRequiresContentMatch") {
    auto rule = parse_permission_rule("Shell(npm:*)", permission_behavior::allow, permission_rule_source::user_settings);
    REQUIRE(matches_rule(rule, "shell", "npm install"));
    REQUIRE_FALSE(matches_rule(rule, "shell", "pip install"));
}

// ── Safety Checks ────────────────────────────────────────────────────────

TEST_CASE("ProtectedPathDetection") {
    REQUIRE(is_protected_path(".git"));
    REQUIRE(is_protected_path(".git/config"));
    REQUIRE(is_protected_path(".orangutan/settings.json"));
    REQUIRE(is_protected_path(".claude/memory"));
    REQUIRE(is_protected_path("/home/user/.bashrc"));
    REQUIRE(is_protected_path("/home/user/.zshrc"));
    REQUIRE(is_protected_path("/home/user/.profile"));
    REQUIRE(is_protected_path("/home/user/.bash_profile"));
}

TEST_CASE("NonProtectedPath") {
    REQUIRE_FALSE(is_protected_path("src/main.cpp"));
    REQUIRE_FALSE(is_protected_path("/tmp/test.txt"));
}

TEST_CASE("SafetyCheckBlocksWriteToProtectedPath") {
    ToolUse call{"id1", "file_edit", {{"file_path", ".git/config"}}};
    auto decision = check_safety(call);
    REQUIRE(decision.has_value());
    REQUIRE(decision->behavior == permission_behavior::ask);
}

TEST_CASE("SafetyCheckAllowsReadOfProtectedPath") {
    ToolUse call{"id1", "file_read", {{"file_path", ".git/HEAD"}}};
    auto decision = check_safety(call);
    REQUIRE_FALSE(decision.has_value());
}

// ── Permission Pipeline ──────────────────────────────────────────────────

TEST_CASE("DenyRuleBlocksTool") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    ctx.deny_rules.push_back(parse_permission_rule("shell", permission_behavior::deny, permission_rule_source::user_settings));

    ToolUse call{"id1", "shell", {{"command", "ls"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::deny);
}

TEST_CASE("AllowRulePermitsTool") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    ctx.allow_rules.push_back(parse_permission_rule("file_read", permission_behavior::allow, permission_rule_source::user_settings));

    ToolUse call{"id1", "file_read", {{"file_path", "src/main.cpp"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("DenyRuleBeatsAllowRule") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    ctx.allow_rules.push_back(parse_permission_rule("shell", permission_behavior::allow, permission_rule_source::user_settings));
    ctx.deny_rules.push_back(parse_permission_rule("shell(rm:*)", permission_behavior::deny, permission_rule_source::user_settings));

    ToolUse call{"id1", "shell", {{"command", "rm -rf /tmp"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::deny);
}

TEST_CASE("BypassModeAutoAllows") {
    ToolPermissionContext ctx{.mode = permission_mode::bypass_permissions};

    ToolUse call{"id1", "shell", {{"command", "ls"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("BypassModeIgnoresDenyRules") {
    ToolPermissionContext ctx{.mode = permission_mode::bypass_permissions};
    ctx.deny_rules.push_back(parse_permission_rule("shell(echo:*)", permission_behavior::deny, permission_rule_source::user_settings));

    ToolUse call{"id1", "shell", {{"command", "echo hello"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("PlanModeAllowsReadOnly") {
    ToolPermissionContext ctx{.mode = permission_mode::plan};

    ToolUse call{"id1", "file_read", {{"file_path", "test.txt"}}};
    auto decision = evaluate_permission(call, ctx, {}, [] { return true; });
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("PlanModeAllowsWorkspaceWriteToolsWhenCheckerPasses") {
    ToolPermissionContext ctx{.mode = permission_mode::plan};
    bool checker_called = false;

    ToolUse call{"id1", "write", {{"path", "test.txt"}, {"content", "hello"}}};
    auto decision = evaluate_permission(
        call, ctx,
        [&checker_called](const ToolUse &, const ToolPermissionContext &) {
            checker_called = true;
            return PermissionResult::passthrough();
        },
        [] {
            return false;
        });

    REQUIRE(checker_called);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("PlanModeDeniesWrite") {
    ToolPermissionContext ctx{.mode = permission_mode::plan};

    ToolUse call{"id1", "shell", {{"command", "echo test"}}};
    auto decision = evaluate_permission(call, ctx, {}, [] { return false; });
    REQUIRE(decision.behavior == permission_behavior::deny);
}

TEST_CASE("DontAskModeConvertsAskToDeny") {
    ToolPermissionContext ctx{.mode = permission_mode::dont_ask};

    ToolUse call{"id1", "shell", {{"command", "ls"}}};
    auto decision = evaluate_permission(call, ctx);
    decision = apply_post_processing(decision, ctx.mode);
    REQUIRE(decision.behavior == permission_behavior::deny);
}

TEST_CASE("DefaultModeAsksForShell") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};

    ToolUse call{"id1", "shell", {{"command", "ls"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::ask);
    REQUIRE(decision.message.has_value());
    CHECK(*decision.message == "Tool 'shell' requires approval");
}

TEST_CASE("DefaultModeAllowsNonSensitiveTools") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};

    ToolUse call{"id1", "task", {{"op", "add"}, {"name", "nightly-sync"}}};
    auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("AcceptEditsAllowsWorkspaceFileToolsWhenCheckerPasses") {
    ToolPermissionContext ctx{.mode = permission_mode::accept_edits};
    bool checker_called = false;

    ToolUse call{"id1", "write", {{"path", "notes.txt"}}};
    auto decision = evaluate_permission(
        call, ctx,
        [&checker_called](const ToolUse &, const ToolPermissionContext &) {
            checker_called = true;
            return PermissionResult::passthrough();
        },
        [] {
            return false;
        });

    REQUIRE(checker_called);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("AcceptEditsStillDeniesWhenFileCheckerRejectsPath") {
    ToolPermissionContext ctx{.mode = permission_mode::accept_edits};

    ToolUse call{"id1", "write", {{"path", "/outside/workspace.txt"}}};
    auto decision = evaluate_permission(
        call, ctx,
        [](const ToolUse &, const ToolPermissionContext &) {
            return PermissionResult::deny("path escapes workspace sandbox");
        },
        [] {
            return false;
        });

    REQUIRE(decision.behavior == permission_behavior::deny);
    REQUIRE(decision.message.has_value());
    CHECK(decision.message->contains("workspace sandbox"));
}

TEST_CASE("EditPatchAllowRuleMatchesCanonicalPathInsteadOfPatchBody") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    ctx.allow_rules.push_back(parse_permission_rule("edit(src/main.cpp)", permission_behavior::allow, permission_rule_source::session));

    ToolUse call{
        "edit-1",
        "edit",
        {
            {"patch", "*** src/main.cpp\n<<<<<<< SEARCH\nold value\n=======\nnew value\n>>>>>>> REPLACE\n"},
        },
    };

    const auto decision = evaluate_permission(call, ctx);
    REQUIRE(decision.behavior == permission_behavior::allow);
}

TEST_CASE("TaskAllowRuleMatchesCanonicalOperationAndName") {
    ToolUse call{
        "task-1",
        "task",
        {
            {"op", "add"},
            {"name", "nightly-sync"},
            {"schedule_kind", "cron"},
            {"schedule", "0 * * * *"},
            {"prompt", "run nightly sync"},
        },
    };

    const auto signature = derive_approval_signature(call);
    CHECK_FALSE(signature.always_allow_eligible);
    CHECK_FALSE(signature.content.has_value());
}

TEST_CASE("ShellCompoundCommandsAreNotEligibleForSessionAlwaysAllow") {
    const ToolUse call{
        "shell-1",
        "shell",
        {
            {"command", "echo hello && git push"},
        },
    };

    const auto signature = derive_approval_signature(call);
    CHECK_FALSE(signature.always_allow_eligible);
    CHECK(signature.downgrade_reason.contains("compound"));
    REQUIRE(signature.content.has_value());
    CHECK(signature.content->pattern == "echo hello && git push");
    CHECK_FALSE(make_session_allow_rule(call).has_value());
}

// ── Permission State ─────────────────────────────────────────────────────

TEST_CASE("InitializeContextFromConfig") {
    PermissionConfig config{
        .default_mode = permission_mode::accept_edits,
        .allow = {"file_read", "grep"},
        .deny = {"shell(rm:*)"},
    };

    auto ctx = initialize_permission_context(config);
    REQUIRE(ctx.mode == permission_mode::accept_edits);
    REQUIRE(ctx.allow_rules.size() == 2);
    REQUIRE(ctx.deny_rules.size() == 1);
}

TEST_CASE("CLIOverridesConfigMode") {
    PermissionConfig config{.default_mode = permission_mode::default_mode};
    CLIPermissionOptions cli{.mode_override = permission_mode::bypass_permissions};

    auto ctx = initialize_permission_context(config, cli);
    REQUIRE(ctx.mode == permission_mode::bypass_permissions);
}

TEST_CASE("DangerouslySkipOverridesAll") {
    PermissionConfig config{.default_mode = permission_mode::default_mode};
    CLIPermissionOptions cli{
        .mode_override = permission_mode::accept_edits,
        .dangerously_skip_permissions = true,
    };

    auto ctx = initialize_permission_context(config, cli);
    REQUIRE(ctx.mode == permission_mode::bypass_permissions);
}

TEST_CASE("ImmutableContextUpdate") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    auto updated = change_mode(ctx, permission_mode::accept_edits);
    REQUIRE(ctx.mode == permission_mode::default_mode);
    REQUIRE(updated.mode == permission_mode::accept_edits);
}

TEST_CASE("AddRuleCreatesNewContext") {
    ToolPermissionContext ctx{.mode = permission_mode::default_mode};
    auto rule = parse_permission_rule("shell", permission_behavior::allow, permission_rule_source::session);
    auto updated = add_rule(ctx, rule);
    REQUIRE(ctx.allow_rules.empty());
    REQUIRE(updated.allow_rules.size() == 1);
}

TEST_CASE("LoadRulesFromNonexistentFile") {
    auto rules = load_rules_from_file("/nonexistent/path.json", permission_rule_source::project_settings);
    REQUIRE(rules.empty());
}

TEST_CASE("LoadRulesFromValidFile") {
    auto temp_dir = std::filesystem::temp_directory_path() / "orangutan_test_perms";
    std::filesystem::create_directories(temp_dir);
    auto settings_file = temp_dir / "test_settings.json";

    {
        std::ofstream out(settings_file);
        out << R"json({"permissions": {"allow": ["file_read"], "deny": ["shell(rm:*)"], "ask": ["shell"]}})json";
    }

    auto rules = load_rules_from_file(settings_file, permission_rule_source::project_settings);
    REQUIRE(rules.size() == 3);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("PersistAndLoadRule") {
    auto temp_dir = std::filesystem::temp_directory_path() / "orangutan_test_persist";
    std::filesystem::create_directories(temp_dir);
    auto settings_file = temp_dir / "persist_test.json";

    auto rule = parse_permission_rule("shell(git:*)", permission_behavior::allow, permission_rule_source::user_settings);
    persist_rule(rule, settings_file);

    auto loaded = load_rules_from_file(settings_file, permission_rule_source::user_settings);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].tool_name == "shell");
    REQUIRE(loaded[0].behavior == permission_behavior::allow);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("PermissionDecisionFormatsRuleReasonForDisplay") {
    const auto decision =
        PermissionDecision::ask_by_rule(permission_rule_source::project_settings, "shell(git push *)", "Shell command approval required.");

    const auto lines = permission_decision_detail_lines(decision);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "Behavior: ask");
    CHECK(lines[1] == "Reason: rule from project settings");
    CHECK(lines[2] == "Rule: shell(git push *)");
}

TEST_CASE("PermissionDecisionSerializesToStructuredJson") {
    const auto decision = PermissionDecision::ask_by_safety(".git/config", "Protected path approval required.");
    const auto json = permission_decision_to_json(decision);

    CHECK(json["behavior"] == "ask");
    CHECK(json["message"] == "Protected path approval required.");
    REQUIRE(json.contains("reason"));
    CHECK(json["reason"]["type"] == "safety_check");
    CHECK(json["reason"]["path"] == ".git/config");
}
