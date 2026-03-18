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
