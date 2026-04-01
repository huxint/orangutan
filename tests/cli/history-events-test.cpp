#include "cli/history-events.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    TEST_CASE("build_edit_details_produces_unified_diff") {
        ToolUse call("edit-1", "edit", {{"path", "src/file.cpp"}, {"old_text", "line1\nold\nline3"}, {"new_text", "line1\nnew\nline3"}});

        const auto details = cli::build_edit_details(call);
        INFO("expected edit details to be a JSON object");
        REQUIRE(details.is_object());
        CHECK(details["type"] == "edit");
        CHECK(details["path"] == "src/file.cpp");
        const auto unified = details["diff"]["unified"].get<std::string>();
        CHECK(unified.contains("-old"));
        CHECK(unified.contains("+new"));
    };

    TEST_CASE("build_session_history_events_includes_tool_lifecycle_and_details") {
        const std::vector<Message> history{
            Message::user().text("hello"),
            Message(base::role::assistant, {ToolUse("edit-1", "edit", nlohmann::json{{"path", "a.txt"}, {"old_text", "a"}, {"new_text", "b"}})}),
            Message(base::role::user, {ToolResult("edit-1", "done", false)}),
        };

        const auto events = cli::build_session_history_events(history);
        CHECK(events.size() >= 5UL);
        CHECK(events.front()["type"] == "session_history_started");
        CHECK(events[1]["type"] == "history_message");
        CHECK(events[2]["type"] == "tool_started");
        CHECK(events[3]["type"] == "tool_finished");
        CHECK(events[3]["details"].is_object());
        CHECK(events.back()["type"] == "session_history_finished");
    };

} // namespace
