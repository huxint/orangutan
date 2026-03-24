#include "app/history-events.hpp"

#include "support/ut.hpp"

using namespace orangutan;

namespace {

boost::ut::suite history_events_suite = [] {
    using namespace boost::ut;

    "build_edit_details_produces_unified_diff"_test = [] {
        ToolUseBlock call{
            .id = "edit-1",
            .name = "edit",
            .input = {{"path", "src/file.cpp"}, {"old_text", "line1\nold\nline3"}, {"new_text", "line1\nnew\nline3"}},
        };

        const auto details = app::build_edit_details(call);
        expect(details.is_object() >> fatal) << "expected edit details to be a JSON object";
        expect(details["type"] == "edit");
        expect(details["path"] == "src/file.cpp");
        const auto unified = details["diff"]["unified"].get<std::string>();
        expect(unified.find("-old") != std::string::npos);
        expect(unified.find("+new") != std::string::npos);
    };

    "build_session_history_events_includes_tool_lifecycle_and_details"_test = [] {
        const std::vector<Message> history{
            Message::user_text("hello"),
            {.role = "assistant", .content = {ToolUseBlock{.id = "edit-1", .name = "edit", .input = {{"path", "a.txt"}, {"old_text", "a"}, {"new_text", "b"}}}}},
            {.role = "user", .content = {ToolResultBlock{.tool_use_id = "edit-1", .content = "done", .is_error = false}}},
        };

        const auto events = app::build_session_history_events(history);
        expect(events.size() >= 5_ul);
        expect(events.front()["type"] == "session_history_started");
        expect(events[1]["type"] == "history_message");
        expect(events[2]["type"] == "tool_started");
        expect(events[3]["type"] == "tool_finished");
        expect(events[3]["details"].is_object());
        expect(events.back()["type"] == "session_history_finished");
    };
};

} // namespace
