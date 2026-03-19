#include "app/cli-ui.hpp"

#include "core/tools/tool.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

TEST(CliUiTest, FormatAgentListMarksCurrentAgentAndSubagents) {
    Config cfg;
    cfg.agents.insert_or_assign("default", AgentConfig{.model = "model-a", .workspace = "/tmp/default", .subagents = {"coder"}});
    cfg.agents.insert_or_assign("coder", AgentConfig{.model = "model-b", .workspace = "/tmp/coder", .subagents = {}});

    const auto text = app::format_agent_list(cfg, "coder");
    EXPECT_NE(text.find("default"), std::string::npos);
    EXPECT_NE(text.find("coder [current]"), std::string::npos);
    EXPECT_NE(text.find("subagents=coder"), std::string::npos);
}

TEST(CliUiTest, RenderHistorySummaryIncludesToolMarkers) {
    class NoopProvider final : public Provider {
    public:
        LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
            return {};
        }
        LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
            return {};
        }
        std::string name() const override {
            return "noop";
        }
    } provider;

    ToolRegistry tools;
    AgentLoop agent(provider, tools);
    agent.set_history({
        Message::user_text("hello"),
        {.role = "assistant", .content = {ToolUseBlock{.id = "1", .name = "read", .input = json::object()}}},
        {.role = "user", .content = {ToolResultBlock{.tool_use_id = "1", .content = "ok", .is_error = false}}},
    });

    const auto text = app::render_history_summary(agent);
    EXPECT_NE(text.find("[0] user: hello"), std::string::npos);
    EXPECT_NE(text.find("[tool_use: read]"), std::string::npos);
    EXPECT_NE(text.find("[tool_result]"), std::string::npos);
}

TEST(CliUiTest, FormatRuntimeStatusIncludesActiveModelFallbacksAndUsage) {
    class StatusProvider final : public Provider {
    public:
        LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
            return {};
        }

        LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
            return {};
        }

        std::string name() const override {
            return "openai";
        }

        std::string current_model() const override {
            return "gpt-fallback";
        }

        ProviderUsageStats usage() const override {
            return ProviderUsageStats{
                .logical_requests = 4,
                .attempt_count = 5,
                .failed_attempts = 1,
                .fallback_switches = 1,
            };
        }
    } provider;

    ToolRegistry tools;
    tools.register_tool({
        .definition = {.name = "shell", .description = "run shell", .input_schema = json::object()},
        .execute =
            [](const json &) {
                return std::string{"ok"};
            },
    });

    AgentLoop agent(provider, tools);
    agent.set_history({
        Message::user_text("hello"),
        {.role = "assistant", .content = {ToolUseBlock{.id = "call-1", .name = "shell", .input = json{{"command", "pwd"}}}}},
        {.role = "user", .content = {ToolResultBlock{.tool_use_id = "call-1", .content = "permission denied", .is_error = true}}},
        Message::assistant_text("done"),
    });

    const auto status = app::collect_runtime_status(agent, provider, &tools, "session-1", "coder", "gpt-primary", {"gpt-fallback"}, "scope:coder");
    EXPECT_EQ(status.current_model, "gpt-fallback");
    EXPECT_EQ(status.configured_model, "gpt-primary");
    EXPECT_EQ(status.tool_call_count, 1U);
    EXPECT_EQ(status.tool_error_count, 1U);
    EXPECT_EQ(status.registered_tool_count, 1U);

    const auto text = app::format_runtime_status(status);
    EXPECT_NE(text.find("📊 Status:"), std::string::npos);
    EXPECT_NE(text.find("provider: openai"), std::string::npos);
    EXPECT_NE(text.find("model: gpt-fallback"), std::string::npos);
    EXPECT_NE(text.find("configured model: gpt-primary"), std::string::npos);
    EXPECT_NE(text.find("fallback models: gpt-fallback"), std::string::npos);
    EXPECT_NE(text.find("usage: llm_requests=4, attempts=5, fallbacks=1, failed_attempts=1, tool_calls=1, tool_errors=1"), std::string::npos);
    EXPECT_NE(text.find("tools: registered=1"), std::string::npos);
    EXPECT_NE(app::format_current_session("session-1", "coder").find("🧵 Current session:"), std::string::npos);
    EXPECT_NE(app::format_current_session("", "coder").find("💤 No active session"), std::string::npos);
    EXPECT_NE(app::repl_help_text().find("/status"), std::string::npos);
    EXPECT_NE(app::channel_help_text().find("/status"), std::string::npos);
}
