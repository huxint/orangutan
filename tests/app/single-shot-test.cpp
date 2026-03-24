#include "app/single-shot.hpp"

#include "infra/storage/session-store.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include "support/ut.hpp"

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

class SingleShotHarness {
public:
    SingleShotHarness()
    : session_db_path_(orangutan::testing::unique_test_db_path("single-shot", "sessions.db")) {}

    ~SingleShotHarness() {
        std::filesystem::remove_all(session_db_path_.parent_path());
    }

    [[nodiscard]]
    const std::filesystem::path &session_db_path() const {
        return session_db_path_;
    }

private:
    std::filesystem::path session_db_path_;
};

boost::ut::suite single_shot_suite = [] {
    using namespace boost::ut;

    "run_single_message_emits_events_and_autosaves_session"_test = [] {
        SingleShotHarness harness;
        StreamingProvider provider;
        ToolRegistry tools;
        AgentLoop agent(provider, tools);
        SessionStore store(harness.session_db_path().string());
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

        expect(status == 0_i);
        expect(not current_session_id.empty());
        expect(events.size() >= 3_ul);
        expect(events[0]["type"] == "assistant_delta");
        expect(events[1]["type"] == "session_saved");
        expect(events[2]["type"] == "done");

        const auto sessions = store.list_sessions("scope:test");
        expect(sessions.size() == 1_ul);
        expect(sessions[0].model == "test-model");
        expect(sessions[0].agent_key == "default");
        expect(sessions[0].origin_kind == "cli");
        expect(sessions[0].origin_ref == "cli:local");
    };

    "run_single_message_uses_distinct_tool_call_and_tool_execution_events"_test = [] {
        SingleShotHarness harness;
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
        SessionStore store(harness.session_db_path().string());
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

        expect(status == 0_i);

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

        expect(tool_call_started_count == 1_ul);
        expect(tool_started_count == 1_ul);
    };
};

boost::ut::suite single_shot_standalone_suite = [] {
    using namespace boost::ut;

    "emit_session_history_dump_wraps_history_with_lifecycle_events"_test = [] {
        std::vector<json> events;
        app::emit_session_history_dump({Message::user_text("hello")}, "session-1", [&events](const json &event) {
            events.push_back(event);
        });

        expect(events.size() == 5_ul);
        expect(events[0]["type"] == "session_resumed");
        expect(events[1]["type"] == "session_history_started");
        expect(events[2]["type"] == "history_message");
        expect(events[3]["type"] == "session_history_finished");
        expect(events[4]["type"] == "done");
    };
};

} // namespace
