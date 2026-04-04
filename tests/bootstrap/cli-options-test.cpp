#include "bootstrap/cli-options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>

namespace {

    using namespace orangutan;
    using namespace orangutan::bootstrap;

    TEST_CASE("permission_flags_parse_from_cli_and_build_permission_options") {
        CliOptions options;
        CLI::App app{"test"};
        CLI::Option *resume_flag = nullptr;
        CLI::Option *protect_flag = nullptr;

        configure_cli_app(app, options, resume_flag, protect_flag);
        std::array<const char *, 9> argv = {
            "test",
            "--cli",
            "--permission-mode",
            "accept-edits",
            "--dangerously-skip-permissions",
            "--allowed-tools",
            "read,shell(git:*),task(list)",
            "--disallowed-tools",
            "write,edit",
        };
        app.parse(static_cast<int>(argv.size()), argv.data());

        const auto cli_permissions = build_cli_permission_options(options);
        REQUIRE(cli_permissions.permission_mode.has_value());
        CHECK(*cli_permissions.permission_mode == PermissionMode::accept_edits);
        CHECK(cli_permissions.dangerously_skip_permissions);
        CHECK(cli_permissions.allowed_tools == std::vector<std::string>{"read", "shell(git:*)", "task(list)"});
        CHECK(cli_permissions.disallowed_tools == std::vector<std::string>{"write", "edit"});
    };

    TEST_CASE("unknown_permission_mode_is_ignored") {
        CliOptions options;
        options.permission_mode_str = "definitely-not-a-mode";

        const auto cli_permissions = build_cli_permission_options(options);
        CHECK_FALSE(cli_permissions.permission_mode.has_value());
    };

    TEST_CASE("cli_permission_prompt_includes_decision_reason_details") {
        const auto prompt = format_cli_permission_prompt(
            ToolUse("approval-shell", "shell", nlohmann::json{{"command", "git push origin main"}}),
            PermissionDecision::ask_by_rule(PermissionRuleSource::project_settings, "shell(git push *)", "Shell command approval required."));

        CHECK(prompt.contains("Shell command approval required."));
        CHECK(prompt.contains("Tool: shell"));
        CHECK(prompt.contains("Behavior: ask"));
        CHECK(prompt.contains("Reason: rule from project settings"));
        CHECK(prompt.contains("Rule: shell(git push *)"));
    };

} // namespace
