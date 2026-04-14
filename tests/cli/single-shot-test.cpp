#include "cli/single-shot.hpp"

#include "storage/session-store.hpp"
#include "test-helpers.hpp"
#include "test-provider-support.hpp"
#include "tools/registry/tool.hpp"

#include <filesystem>
#include <iostream>
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    std::shared_ptr<orangutan::testing::FakeProviderBackend> make_streaming_backend() {
        auto backend = orangutan::testing::make_fake_provider_backend(
            [](const providers::ProviderRoute &route, const providers::ProviderRequest &, const providers::ProviderEventSink &sink) {
                if (sink != nullptr) {
                    sink(providers::TextDelta{.text = "hello"});
                }
                return providers::ProviderResult{
                    .response = orangutan::testing::make_text_response("hello"),
                    .usage_snapshot = {},
                    .active_target = route.primary,
                };
            });
        backend->set_label("streaming-provider");
        return backend;
    }

    std::shared_ptr<orangutan::testing::FakeProviderBackend> make_tool_streaming_backend() {
        auto tool_round_completed = std::make_shared<bool>(false);
        auto backend = orangutan::testing::make_fake_provider_backend(
            [tool_round_completed](const providers::ProviderRoute &route, const providers::ProviderRequest &, const providers::ProviderEventSink &sink) {
                if (!*tool_round_completed) {
                    if (sink != nullptr) {
                        sink(providers::ToolCallStarted{
                            .id = "tool-1",
                            .name = "fake_tool",
                            .input = nlohmann::json{{"value", 1}},
                        });
                    }
                    *tool_round_completed = true;
                    return providers::ProviderResult{
                        .response =
                            LLMResponse{
                                .stop_reason = response_stop_reason::tool_use,
                                .content = {ToolUse("tool-1", "fake_tool", nlohmann::json{{"value", 1}})},
                            },
                        .usage_snapshot = {},
                        .active_target = route.primary,
                    };
                }

                if (sink != nullptr) {
                    sink(providers::TextDelta{.text = "done"});
                }
                return providers::ProviderResult{
                    .response = orangutan::testing::make_text_response("done"),
                    .usage_snapshot = {},
                    .active_target = route.primary,
                };
            });
        backend->set_label("tool-streaming-provider");
        return backend;
    }

    class SingleShotHarness {
    public:
        SingleShotHarness()
        : session_db_path_(orangutan::testing::unique_test_db_path("single-shot", "sessions.db")) {}

        ~SingleShotHarness() {
            std::filesystem::remove_all(session_db_path_.parent_path());
        }
        SingleShotHarness(const SingleShotHarness &) = delete;
        SingleShotHarness &operator=(const SingleShotHarness &) = delete;
        SingleShotHarness(SingleShotHarness &&) = delete;
        SingleShotHarness &operator=(SingleShotHarness &&) = delete;

        [[nodiscard]]
        const std::filesystem::path &session_db_path() const {
            return session_db_path_;
        }

    private:
        std::filesystem::path session_db_path_;
    };

    TEST_CASE("run_single_message_emits_events_and_autosaves_session") {
        SingleShotHarness harness;
        auto provider_backend = make_streaming_backend();
        auto provider = orangutan::testing::make_provider_system(provider_backend);
        const auto route = orangutan::testing::make_test_route("test-model");
        ToolRegistry tools;
        AgentLoop agent(provider, route, tools);
        SessionStore store(harness.session_db_path());
        Config cfg;
        cfg.auto_save = true;

        std::vector<nlohmann::json> events;
        std::string current_session_id;
        const auto status = cli::run_single_message(
            agent, provider, store, cfg, "hello", true, current_session_id, "test-model", "scope:test", "default",
            [&events](const nlohmann::json &event) {
                events.push_back(event);
            },
            std::cerr);

        CHECK(status == 0);
        CHECK_FALSE(current_session_id.empty());
        CHECK(events.size() >= 3UL);
        CHECK(events[0]["type"] == "assistant_delta");
        CHECK(events[1]["type"] == "session_saved");
        CHECK(events[2]["type"] == "done");

        const auto sessions = store.list_sessions("scope:test");
        CHECK(sessions.size() == 1UL);
        CHECK(sessions[0].model == "test-model");
        CHECK(sessions[0].agent_key == "default");
        CHECK(sessions[0].origin_kind == "cli");
        CHECK(sessions[0].origin_ref == "cli:local");
    };

    TEST_CASE("run_single_message_uses_distinct_tool_call_and_tool_execution_events") {
        SingleShotHarness harness;
        auto provider_backend = make_tool_streaming_backend();
        auto provider = orangutan::testing::make_provider_system(provider_backend);
        const auto route = orangutan::testing::make_test_route("test-model");
        ToolRegistry tools;
        tools.register_tool({
            .definition = {.name = "fake_tool", .description = "fake", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return std::string{"ok"};
                },
        });
        AgentLoop agent(provider, route, tools);
        SessionStore store(harness.session_db_path());
        Config cfg;
        cfg.auto_save = false;

        std::vector<nlohmann::json> events;
        std::string current_session_id;
        const auto status = cli::run_single_message(
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

        CHECK(tool_call_started_count == 1UL);
        CHECK(tool_started_count == 1UL);
    };

    TEST_CASE("emit_session_history_dump_wraps_history_with_lifecycle_events") {
        std::vector<nlohmann::json> events;
        cli::emit_session_history_dump({Message::user().text("hello")}, "session-1", [&events](const nlohmann::json &event) {
            events.push_back(event);
        });

        CHECK(events.size() == 5UL);
        CHECK(events[0]["type"] == "session_resumed");
        CHECK(events[1]["type"] == "session_history_started");
        CHECK(events[2]["type"] == "history_message");
        CHECK(events[3]["type"] == "session_history_finished");
        CHECK(events[4]["type"] == "done");
    };

} // namespace
