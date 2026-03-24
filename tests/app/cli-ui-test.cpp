#include "app/cli-ui.hpp"

#include "core/tools/tool.hpp"
#include "support/ut.hpp"

namespace {

boost::ut::suite cli_ui_suite = [] {
    using namespace boost::ut;

    "format_agent_list_marks_current_agent_and_subagents"_test = [] {
        orangutan::Config cfg;
        cfg.agents.insert_or_assign("default", orangutan::AgentConfig{.model = "model-a", .workspace = "/tmp/default", .subagents = {"coder"}});
        cfg.agents.insert_or_assign("coder", orangutan::AgentConfig{.model = "model-b", .workspace = "/tmp/coder", .subagents = {}});

        const auto text = orangutan::app::format_agent_list(cfg, "coder");
        expect(text.find("## Agents") != std::string::npos);
        expect(text.find("`default`") != std::string::npos);
        expect(text.find("`coder` **(current)**") != std::string::npos);
        expect(text.find("subagents: `coder`") != std::string::npos);
    };

    "format_runtime_status_includes_active_model_fallbacks_and_usage"_test = [] {
        class StatusProvider final : public orangutan::Provider {
        public:
            orangutan::LLMResponse chat(std::string_view, const std::vector<orangutan::Message> &, const std::vector<orangutan::ToolDef> &, int) override {
                return {};
            }

            orangutan::LLMResponse chat_stream(std::string_view, const std::vector<orangutan::Message> &, const std::vector<orangutan::ToolDef> &,
                                               const orangutan::StreamCallback &, int) override {
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
            {.role = orangutan::Role::Assistant, .content = {orangutan::ToolUseBlock{.id = "call-1", .name = "shell", .input = nlohmann::json{{"command", "pwd"}}}}},
            {.role = orangutan::Role::User, .content = {orangutan::ToolResultBlock{.tool_use_id = "call-1", .content = "permission denied", .is_error = true}}},
            orangutan::Message::assistant_text("done"),
        });

        const auto status = orangutan::app::collect_runtime_status(agent, provider, &tools, "session-1", "coder", "gpt-primary", {"gpt-fallback"}, "scope:coder");
        expect(status.current_model == "gpt-fallback");
        expect(status.configured_model == "gpt-primary");
        expect(status.tool_call_count == 1_ul);
        expect(status.tool_error_count == 1_ul);
        expect(status.registered_tool_count == 1_ul);

        const auto text = orangutan::app::format_runtime_status(status);
        expect(text.find("## Status") != std::string::npos);
        expect(text.find("- 🔌 Provider: `openai`") != std::string::npos);
        expect(text.find("- 🧠 Model: `gpt-fallback`") != std::string::npos);
        expect(text.find("- 🎯 Configured Model: `gpt-primary`") != std::string::npos);
        expect(text.find("- 🔁 Fallback Models: `gpt-fallback`") != std::string::npos);
        expect(text.find("- 📊 Usage: `llm_requests=4`, `attempts=5`, `fallbacks=1`, `failed_attempts=1`, `tool_calls=1`, `tool_errors=1`") != std::string::npos);
        expect(text.find("- 🛠️ Tools: `registered=1`") != std::string::npos);
        expect(orangutan::app::format_current_session("session-1", "coder").find("## Session") != std::string::npos);
        expect(orangutan::app::format_current_session("session-1", "coder").find("🧵 Current Session: `session-1`") != std::string::npos);
        expect(orangutan::app::format_current_session("", "coder").find("💤 No active session") != std::string::npos);
        expect(orangutan::app::format_current_agent("coder").find("## Agent") != std::string::npos);
        expect(orangutan::app::format_current_agent("coder").find("🤖 Current Agent: `coder`") != std::string::npos);
        expect(orangutan::app::repl_help_text().find("## Commands") != std::string::npos);
        expect(orangutan::app::repl_help_text().find("`/status`") != std::string::npos);
        expect(orangutan::app::repl_help_text().find("`/export`") != std::string::npos);
        expect(orangutan::app::repl_help_text().find("`/history`") == std::string::npos);
        expect(orangutan::app::repl_help_text().find("`/load`") == std::string::npos);
        expect(orangutan::app::channel_help_text().find("## Commands") != std::string::npos);
        expect(orangutan::app::channel_help_text().find("`/status`") != std::string::npos);
        expect(orangutan::app::channel_help_text().find("`/export`") != std::string::npos);
        expect(orangutan::app::channel_help_text().find("`/history`") == std::string::npos);
        expect(orangutan::app::channel_help_text().find("`/load`") == std::string::npos);
        expect(orangutan::app::web_help_text().find("## Commands") != std::string::npos);
        expect(orangutan::app::web_help_text().find("`/status`") != std::string::npos);
        expect(orangutan::app::web_help_text().find("`/export`") != std::string::npos);
        expect(orangutan::app::web_help_text().find("`/history`") == std::string::npos);
        expect(orangutan::app::web_help_text().find("`/load`") == std::string::npos);

        const auto compacted =
            orangutan::app::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{.compacted = true, .messages_before = 20, .messages_after = 8});
        expect(compacted == "## Compression\n- Messages: `20 -> 8`");
        expect(orangutan::app::format_history_compaction_result(orangutan::AgentLoop::HistoryCompactionResult{
                   .compacted = false, .status = "Not enough history to compress yet."}) == "## Compression\n- Not enough history to compress yet.");
    };
};

} // namespace
