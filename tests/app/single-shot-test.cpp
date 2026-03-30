#include "app/single-shot.hpp"

#include "infra/storage/session-store.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <iostream>
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    class StreamingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            return {
                .stop_reason = "end_turn",
                .content = {Text{"hello"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &on_event, int, int = 0) override {
            on_event("text_delta", nlohmann::json{{"text", "hello"}});
            return {
                .stop_reason = "end_turn",
                .content = {Text{"hello"}},
            };
        }

        std::string name() const override {
            return "streaming-provider";
        }
    };

    class ToolStreamingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            return {
                .stop_reason = "end_turn",
                .content = {Text{"done"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &on_event, int, int = 0) override {
            if (!tool_round_completed_) {
                on_event("tool_call_start", nlohmann::json{{"id", "tool-1"}, {"name", "fake_tool"}, {"input", nlohmann::json{{"value", 1}}}});
                tool_round_completed_ = true;
                return {
                    .stop_reason = "tool_use",
                    .content = {ToolUse("tool-1", "fake_tool", nlohmann::json{{"value", 1}})},
                };
            }

            on_event("text_delta", nlohmann::json{{"text", "done"}});
            return {
                .stop_reason = "end_turn",
                .content = {Text{"done"}},
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

    TEST_CASE("run_single_message_emits_events_and_autosaves_session") {
        SingleShotHarness harness;
        StreamingProvider provider;
        ToolRegistry tools;
        AgentLoop agent(provider, tools);
        SessionStore store(harness.session_db_path());
        Config cfg;
        cfg.auto_save = true;

        std::vector<nlohmann::json> events;
        std::string current_session_id;
        const auto status = app::run_single_message(
            agent, provider, store, cfg, "hello", true, current_session_id, "test-model", "scope:test", "default",
            [&events](const nlohmann::json &event) {
                events.push_back(event);
            },
            std::cerr);

        CHECK(status == 0);
        CHECK_FALSE(current_session_id.empty());
        CHECK(events.size() >= 3ul);
        CHECK(events[0]["type"] == "assistant_delta");
        CHECK(events[1]["type"] == "session_saved");
        CHECK(events[2]["type"] == "done");

        const auto sessions = store.list_sessions("scope:test");
        CHECK(sessions.size() == 1ul);
        CHECK(sessions[0].model == "test-model");
        CHECK(sessions[0].agent_key == "default");
        CHECK(sessions[0].origin_kind == "cli");
        CHECK(sessions[0].origin_ref == "cli:local");
    };

    TEST_CASE("run_single_message_uses_distinct_tool_call_and_tool_execution_events") {
        SingleShotHarness harness;
        ToolStreamingProvider provider;
        ToolRegistry tools;
        tools.register_tool({
            .definition = {.name = "fake_tool", .description = "fake", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return std::string{"ok"};
                },
        });
        AgentLoop agent(provider, tools);
        SessionStore store(harness.session_db_path());
        Config cfg;
        cfg.auto_save = false;

        std::vector<nlohmann::json> events;
        std::string current_session_id;
        const auto status = app::run_single_message(
            agent, provider, store, cfg, "run tool", true, current_session_id, "test-model", "scope:test", "default",
            [&events](const nlohmann::json &event) {
                events.push_back(event);
            },
            std::cerr);

        CHECK(status == 0);

        std::size_t tool_call_started_count = 0;
        std::size_t tool_started_count = 0;
        for (const auto &event : events) {
            const auto type = event.at("type").get<std::string>();
            if (type == "tool_call_started") {
                ++tool_call_started_count;
            }
            if (type == "tool_started") {
                ++tool_started_count;
            }
        }

        CHECK(tool_call_started_count == 1ul);
        CHECK(tool_started_count == 1ul);
    };

    TEST_CASE("emit_session_history_dump_wraps_history_with_lifecycle_events") {
        std::vector<nlohmann::json> events;
        app::emit_session_history_dump({Message::user().text("hello")}, "session-1", [&events](const nlohmann::json &event) {
            events.push_back(event);
        });

        CHECK(events.size() == 5ul);
        CHECK(events[0]["type"] == "session_resumed");
        CHECK(events[1]["type"] == "session_history_started");
        CHECK(events[2]["type"] == "history_message");
        CHECK(events[3]["type"] == "session_history_finished");
        CHECK(events[4]["type"] == "done");
    };

} // namespace
