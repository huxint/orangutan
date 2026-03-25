#include "app/cli-ui.hpp"

#include "core/tools/tool.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("format_agent_list_marks_current_agent_and_subagents") {
    orangutan::Config cfg;
    cfg.agents.insert_or_assign("default", orangutan::AgentConfig{.model = "model-a", .workspace = "/tmp/default", .subagents = {"coder"}});
    cfg.agents.insert_or_assign("coder", orangutan::AgentConfig{.model = "model-b", .workspace = "/tmp/coder", .subagents = {}});

    const auto text = orangutan::app::format_agent_list(cfg, "coder");
    CHECK(text.contains("## Agents"));
    CHECK(text.contains("`default`"));
    CHECK(text.contains("`coder` **(current)**"));
    CHECK(text.contains("subagents: `coder`"));
};

TEST_CASE("format_runtime_status_includes_active_model_fallbacks_and_usage") {
    class StatusProvider final : public orangutan::Provider {
    public:
        orangutan::LLMResponse chat(std::string_view, const std::vector<orangutan::Message> &, const std::vector<orangutan::ToolDef> &, int) override {
            return {};
        }

        orangutan::LLMResponse chat_stream(std::string_view, const std::vector<orangutan::Message> &, const std::vector<orangutan::ToolDef> &, const orangutan::StreamCallback &,
                                           int) override {
            return {};
        }

        std::string name() const override {
            return "openai";
        }

        std::string current_model() const override {
            return "gpt-fallback";
        }

        orangutan::ProviderUsageStats usage() const override {
            return orangutan::ProviderUsageStats{
                .logical_requests = 4,
                .attempt_count = 5,
                .failed_attempts = 1,
                .fallback_switches = 1,
            };
        }
    } provider;

    orangutan::ToolRegistry tools;
    tools.register_tool({
        .definition = {.name = "shell", .description = "run shell", .input_schema = nlohmann::json::object()},
        .execute =
            [](const nlohmann::json &) {
                return std::string{"ok"};
            },
    });

    orangutan::AgentLoop agent(provider, tools);
    agent.set_history({
        orangutan::Message::user_text("hello"),
        {.role = orangutan::Role::assistant, .content = {orangutan::ToolUseBlock{.id = "call-1", .name = "shell", .input = nlohmann::json{{"command", "pwd"}}}}},
        {.role = orangutan::Role::user, .content = {orangutan::ToolResultBlock{.tool_use_id = "call-1", .content = "permission denied", .is_error = true}}},
        orangutan::Message::assistant_text("done"),
    });

    const auto status = orangutan::app::collect_runtime_status(agent, provider, &tools, "session-1", "coder", "gpt-primary", {"gpt-fallback"}, "scope:coder");
    CHECK(status.current_model == "gpt-fallback");
    CHECK(status.configured_model == "gpt-primary");
    CHECK(status.tool_call_count == 1ul);
    CHECK(status.tool_error_count == 1ul);
    CHECK(status.registered_tool_count == 1ul);

    const auto text = orangutan::app::format_runtime_status(status);
    CHECK(text.contains("## Status"));
    CHECK(text.contains("- 🔌 Provider: `openai`"));
    CHECK(text.contains("- 🧠 Model: `gpt-fallback`"));
    CHECK(text.contains("- 🎯 Configured Model: `gpt-primary`"));
    CHECK(text.contains("- 🔁 Fallback Models: `gpt-fallback`"));
    CHECK(text.contains("- 📊 Usage: `llm_requests=4`, `attempts=5`, `fallbacks=1`, `failed_attempts=1`, `tool_calls=1`, `tool_errors=1`"));
    CHECK(text.contains("- 🛠️ Tools: `registered=1`"));
    CHECK(orangutan::app::format_current_session("session-1", "coder").contains("## Session"));
    CHECK(orangutan::app::format_current_session("session-1", "coder").contains("🧵 Current Session: `session-1`"));
    CHECK(orangutan::app::format_current_session("", "coder").contains("💤 No active session"));
    CHECK(orangutan::app::format_current_agent("coder").contains("## Agent"));
    CHECK(orangutan::app::format_current_agent("coder").contains("🤖 Current Agent: `coder`"));
    CHECK(orangutan::app::repl_help_text().contains("## Commands"));
    CHECK(orangutan::app::repl_help_text().contains("`/status`"));
    CHECK(orangutan::app::repl_help_text().contains("`/export`"));
    CHECK_FALSE(orangutan::app::repl_help_text().contains("`/history`"));
    CHECK_FALSE(orangutan::app::repl_help_text().contains("`/load`"));
    CHECK(orangutan::app::channel_help_text().contains("## Commands"));
    CHECK(orangutan::app::channel_help_text().contains("`/status`"));
    CHECK(orangutan::app::channel_help_text().contains("`/export`"));
    CHECK_FALSE(orangutan::app::channel_help_text().contains("`/history`"));
    CHECK_FALSE(orangutan::app::channel_help_text().contains("`/load`"));
    CHECK(orangutan::app::web_help_text().contains("## Commands"));
    CHECK(orangutan::app::web_help_text().contains("`/status`"));
    CHECK(orangutan::app::web_help_text().contains("`/export`"));
    CHECK_FALSE(orangutan::app::web_help_text().contains("`/history`"));
    CHECK_FALSE(orangutan::app::web_help_text().contains("`/load`"));

    const auto compacted =
        orangutan::app::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{.compacted = true, .messages_before = 20, .messages_after = 8});
    CHECK(compacted == "## Compression\n- Messages: `20 -> 8`");
    CHECK(orangutan::app::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{.compacted = false, .status = "Not enough history to compress yet."}) ==
          "## Compression\n- Not enough history to compress yet.");
};

} // namespace
