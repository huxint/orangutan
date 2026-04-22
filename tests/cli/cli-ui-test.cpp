#include "cli/cli-ui.hpp"

#include "skills/runtime.hpp"
#include "test-provider-support.hpp"
#include "tools/registry/tool.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

    TEST_CASE("format_agent_list_marks_current_agent_and_team_agents") {
        orangutan::Config cfg;
        cfg.agents.insert_or_assign("default", orangutan::AgentConfig{.model = "model-a", .workspace = "/tmp/default", .team_agents = {"coder"}});
        cfg.agents.insert_or_assign("coder", orangutan::AgentConfig{.model = "model-b", .workspace = "/tmp/coder", .team_agents = {}});

        const auto text = orangutan::cli::format_agent_list(cfg, "coder");
        CHECK(text.contains("## Agents"));
        CHECK(text.contains("`default`"));
        CHECK(text.contains("`coder` **(current)**"));
        CHECK(text.contains("team_agents: `coder`"));
    };

    TEST_CASE("collect_and_format_runtime_status_reports_model_usage_and_tool_counts") {
        auto backend = orangutan::testing::make_fake_provider_backend();
        backend->set_label("openai");
        backend->set_active_target(orangutan::testing::make_test_target("gpt-fallback"));
        backend->set_usage(orangutan::providers::ProviderUsageStats{
            .logical_requests = 4,
            .attempt_count = 5,
            .failed_attempts = 1,
            .fallback_switches = 1,
        });
        auto provider = orangutan::testing::make_provider_system(backend);
        const auto route = orangutan::testing::make_test_route("gpt-primary");

        orangutan::ToolRegistry tools;
        tools.register_tool({
            .definition = {.name = "shell", .description = "run shell", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return std::string{"ok"};
                },
        });

        orangutan::AgentLoop agent(provider, route, tools);
        agent.set_history({
            orangutan::Message::user().text("hello"),
            orangutan::Message(orangutan::base::role::assistant, {orangutan::ToolUse("call-1", "shell", nlohmann::json{{"command", "pwd"}})}),
            orangutan::Message(orangutan::base::role::user, {orangutan::ToolResult("call-1", "permission denied", true)}),
            orangutan::Message::assistant().text("done"),
        });

        const auto status = orangutan::cli::collect_runtime_status(agent, provider, &tools, "session-1", "coder", "gpt-primary", {"gpt-fallback"}, "scope:coder");
        CHECK(status.current_model == "gpt-fallback");
        CHECK(status.configured_model == "gpt-primary");
        CHECK(status.tool_call_count == 1UL);
        CHECK(status.tool_error_count == 1UL);
        CHECK(status.registered_tool_count == 1UL);

        const auto text = orangutan::cli::format_runtime_status(status);
        CHECK(text.contains("## Status"));
        CHECK(text.contains("- 🔌 Provider: `openai`"));
        CHECK(text.contains("- 🧠 Model: `gpt-fallback`"));
        CHECK(text.contains("- 🎯 Configured Model: `gpt-primary`"));
        CHECK(text.contains("- 🔁 Fallback Models: `gpt-fallback`"));
        CHECK(text.contains("- 📊 Usage: `llm_requests=4`, `attempts=5`, `fallbacks=1`, `failed_attempts=1`, `tool_calls=1`, `tool_errors=1`"));
        CHECK(text.contains("- 🛠️ Tools: `registered=1`"));
    };

    TEST_CASE("session_and_agent_formatters_render_expected_sections") {
        CHECK(orangutan::cli::format_current_session("session-1", "coder").contains("## Session"));
        CHECK(orangutan::cli::format_current_session("session-1", "coder").contains("🧵 Current Session: `session-1`"));
        CHECK(orangutan::cli::format_current_session("", "coder").contains("💤 No active session"));
        CHECK(orangutan::cli::format_current_agent("coder").contains("## Agent"));
        CHECK(orangutan::cli::format_current_agent("coder").contains("🤖 Current Agent: `coder`"));
    };

    TEST_CASE("help_texts_include_supported_commands_and_hide_removed_commands") {
        CHECK(orangutan::cli::repl_help_text().contains("## Commands"));
        CHECK(orangutan::cli::repl_help_text().contains("`/status`"));
        CHECK(orangutan::cli::repl_help_text().contains("`/export`"));
        CHECK_FALSE(orangutan::cli::repl_help_text().contains("`/history`"));
        CHECK_FALSE(orangutan::cli::repl_help_text().contains("`/load`"));
        CHECK(orangutan::cli::channel_help_text().contains("## Commands"));
        CHECK(orangutan::cli::channel_help_text().contains("`/status`"));
        CHECK(orangutan::cli::channel_help_text().contains("`/export`"));
        CHECK_FALSE(orangutan::cli::channel_help_text().contains("`/history`"));
        CHECK_FALSE(orangutan::cli::channel_help_text().contains("`/load`"));
        CHECK(orangutan::cli::web_help_text().contains("## Commands"));
        CHECK(orangutan::cli::web_help_text().contains("`/status`"));
        CHECK(orangutan::cli::web_help_text().contains("`/export`"));
        CHECK_FALSE(orangutan::cli::web_help_text().contains("`/history`"));
        CHECK_FALSE(orangutan::cli::web_help_text().contains("`/load`"));
    };

    TEST_CASE("format_history_compaction_result_reports_compacted_and_noop_states") {
        const auto compacted =
            orangutan::cli::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{.compacted = true, .messages_before = 20, .messages_after = 8});
        CHECK(compacted == "## Compression\n- Messages: `20 -> 8`");
        CHECK(orangutan::cli::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{
                  .compacted = false, .status = "Not enough history to compress yet."}) == "## Compression\n- Not enough history to compress yet.");
    };

    TEST_CASE("format_skill_catalog_reports_empty_state") {
        const orangutan::skills::skill_catalog_view catalog;

        const auto text = orangutan::cli::format_skill_catalog(catalog);

        CHECK(text == "## Skills\n- 💤 No skills loaded.");
    };

    TEST_CASE("format_skill_catalog_includes_scope_source_and_activity") {
        orangutan::skills::skill_catalog_view catalog;
        catalog.skills.push_back(orangutan::skills::skill_view{
            .name = "beta",
            .description = "inactive conditional skill",
            .source = orangutan::skills::skill_source::workspace,
            .scope = orangutan::skills::skill_scope::conditional,
            .active = false,
            .source_path = "/tmp/beta/SKILL.md",
            .tools = {"read"},
        });
        catalog.skills.push_back(orangutan::skills::skill_view{
            .name = "alpha",
            .description = "active always skill",
            .source = orangutan::skills::skill_source::user,
            .scope = orangutan::skills::skill_scope::always,
            .active = true,
            .source_path = "/tmp/alpha/SKILL.md",
            .tools = {"read", "edit"},
        });

        const auto text = orangutan::cli::format_skill_catalog(catalog);

        CHECK(text.contains("## Skills"));
        CHECK(text.contains("`alpha` — active always skill"));
        CHECK(text.contains("`beta` — inactive conditional skill"));
        CHECK(text.find("`alpha`") < text.find("`beta`"));
        CHECK(text.contains("source: `user`, scope: `always`, active: `yes`"));
        CHECK(text.contains("source: `workspace`, scope: `conditional`, active: `no`"));
        CHECK(text.contains("tools: `read`, `edit`"));
        CHECK(text.contains("path: `/tmp/alpha/SKILL.md`"));
    };

} // namespace
