#include "app/history-events.hpp"

#include <gtest/gtest.h>

using namespace orangutan;

TEST(HistoryEventsTest, BuildEditDetailsProducesUnifiedDiff) {
    ToolUseBlock call{
        .id = "edit-1",
        .name = "edit",
        .input = {{"path", "src/file.cpp"}, {"old_text", "line1\nold\nline3"}, {"new_text", "line1\nnew\nline3"}},
    };

    const auto details = app::build_edit_details(call);
    ASSERT_TRUE(details.is_object());
    EXPECT_EQ(details["type"], "edit");
    EXPECT_EQ(details["path"], "src/file.cpp");
    const auto unified = details["diff"]["unified"].get<std::string>();
    EXPECT_NE(unified.find("-old"), std::string::npos);
    EXPECT_NE(unified.find("+new"), std::string::npos);
}

TEST(HistoryEventsTest, BuildSessionHistoryEventsIncludesToolLifecycleAndDetails) {
    const std::vector<Message> history{
        Message::user_text("hello"),
        {.role = "assistant", .content = {ToolUseBlock{.id = "edit-1", .name = "edit", .input = {{"path", "a.txt"}, {"old_text", "a"}, {"new_text", "b"}}}}},
        {.role = "user", .content = {ToolResultBlock{.tool_use_id = "edit-1", .content = "done", .is_error = false}}},
    };

    const auto events = app::build_session_history_events(history);
    ASSERT_GE(events.size(), 5U);
    EXPECT_EQ(events.front()["type"], "session_history_started");
    EXPECT_EQ(events[1]["type"], "history_message");
    EXPECT_EQ(events[2]["type"], "tool_started");
    EXPECT_EQ(events[3]["type"], "tool_finished");
    EXPECT_TRUE(events[3]["details"].is_object());
    EXPECT_EQ(events.back()["type"], "session_history_finished");
}
