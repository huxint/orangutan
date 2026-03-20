#include "app/single-shot.hpp"

#include "infra/storage/session-store.hpp"
#include "core/tools/tool.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

class StreamingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "hello"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &on_event, int) override {
        on_event("text_delta", json{{"text", "hello"}});
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "hello"}},
        };
    }

    std::string name() const override {
        return "streaming-provider";
    }
};

class ToolStreamingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "done"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &on_event, int) override {
        if (!tool_round_completed_) {
            on_event("tool_call_start", json{{"id", "tool-1"}, {"name", "fake_tool"}, {"input", json{{"value", 1}}}});
            tool_round_completed_ = true;
            return {
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "tool-1", .name = "fake_tool", .input = {{"value", 1}}}},
            };
        }

        on_event("text_delta", json{{"text", "done"}});
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "done"}},
        };
    }

    std::string name() const override {
        return "tool-streaming-provider";
    }

private:
    bool tool_round_completed_ = false;
};

class SingleShotTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_db_path_ = std::filesystem::temp_directory_path() / "orangutan_single_shot_test.db";
        std::filesystem::remove(session_db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(session_db_path_);
    }

    [[nodiscard]]
    const std::filesystem::path &session_db_path() const {
        return session_db_path_;
    }

private:
    std::filesystem::path session_db_path_;
};

} // namespace

TEST_F(SingleShotTest, RunSingleMessageEmitsEventsAndAutosavesSession) {
    StreamingProvider provider;
    ToolRegistry tools;
    AgentLoop agent(provider, tools);
    SessionStore store(session_db_path().string());
    Config cfg;
    cfg.auto_save = true;

    std::vector<json> events;
    std::string current_session_id;
    const auto status = app::run_single_message(
        agent, provider, store, cfg, "hello", true, current_session_id, "test-model", "scope:test", "default",
        [&events](const json &event) {
            events.push_back(event);
        },
        std::cerr);

    EXPECT_EQ(status, 0);
    EXPECT_FALSE(current_session_id.empty());
    ASSERT_GE(events.size(), 3U);
    EXPECT_EQ(events[0]["type"], "assistant_delta");
    EXPECT_EQ(events[1]["type"], "session_saved");
    EXPECT_EQ(events[2]["type"], "done");

    const auto sessions = store.list_sessions("scope:test");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].model, "test-model");
    EXPECT_EQ(sessions[0].agent_key, "default");
    EXPECT_EQ(sessions[0].origin_kind, "cli");
    EXPECT_EQ(sessions[0].origin_ref, "cli:local");
}

TEST_F(SingleShotTest, RunSingleMessageUsesDistinctToolCallAndToolExecutionEvents) {
    ToolStreamingProvider provider;
    ToolRegistry tools;
    tools.register_tool({
        .definition = {.name = "fake_tool", .description = "fake", .input_schema = json::object()},
        .execute =
            [](const json &) {
                return std::string{"ok"};
            },
    });
    AgentLoop agent(provider, tools);
    SessionStore store(session_db_path().string());
    Config cfg;
    cfg.auto_save = false;

    std::vector<json> events;
    std::string current_session_id;
    const auto status = app::run_single_message(
        agent, provider, store, cfg, "run tool", true, current_session_id, "test-model", "scope:test", "default",
        [&events](const json &event) {
            events.push_back(event);
        },
        std::cerr);

    EXPECT_EQ(status, 0);

    size_t tool_call_started_count = 0;
    size_t tool_started_count = 0;
    for (const auto &event : events) {
        const auto type = event.at("type").get<std::string>();
        if (type == "tool_call_started") {
            ++tool_call_started_count;
        }
        if (type == "tool_started") {
            ++tool_started_count;
        }
    }

    EXPECT_EQ(tool_call_started_count, 1U);
    EXPECT_EQ(tool_started_count, 1U);
}

TEST(SingleShotStandaloneTest, EmitSessionHistoryDumpWrapsHistoryWithLifecycleEvents) {
    std::vector<json> events;
    app::emit_session_history_dump({Message::user_text("hello")}, "session-1", [&events](const json &event) {
        events.push_back(event);
    });

    ASSERT_EQ(events.size(), 5U);
    EXPECT_EQ(events[0]["type"], "session_resumed");
    EXPECT_EQ(events[1]["type"], "session_history_started");
    EXPECT_EQ(events[2]["type"], "history_message");
    EXPECT_EQ(events[3]["type"], "session_history_finished");
    EXPECT_EQ(events[4]["type"], "done");
}
