#include "core/types.hpp"

#include <gtest/gtest.h>

using namespace orangutan;

// ── Message factory methods ─────────────────────

TEST(MessageTest, UserTextCreatesCorrectRole) {
    const auto msg = Message::user_text("hello");
    EXPECT_EQ(msg.role, "user");
    ASSERT_EQ(msg.content.size(), 1);

    const auto *text = std::get_if<TextBlock>(&msg.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "hello");
}

TEST(MessageTest, AssistantTextCreatesCorrectRole) {
    const auto msg = Message::assistant_text("hi there");
    EXPECT_EQ(msg.role, "assistant");
    ASSERT_EQ(msg.content.size(), 1);

    const auto *text = std::get_if<TextBlock>(&msg.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "hi there");
}

TEST(MessageTest, EmptyTextIsAllowed) {
    const auto msg = Message::user_text("");
    const auto *text = std::get_if<TextBlock>(&msg.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(text->text.empty());
}

// ── content_block_to_json ───────────────────────

TEST(ContentBlockToJsonTest, SerializesTextBlock) {
    const ContentBlock block = TextBlock{"hello world"};
    const json j = content_block_to_json(block);

    EXPECT_EQ(j["type"], "text");
    EXPECT_EQ(j["text"], "hello world");
}

TEST(ContentBlockToJsonTest, SerializesToolUseBlock) {
    const ContentBlock block = ToolUseBlock{
        .id = "call_123",
        .name = "shell",
        .input = {{"command", "ls"}},
    };
    const json j = content_block_to_json(block);

    EXPECT_EQ(j["type"], "tool_use");
    EXPECT_EQ(j["id"], "call_123");
    EXPECT_EQ(j["name"], "shell");
    EXPECT_EQ(j["input"]["command"], "ls");
}

TEST(ContentBlockToJsonTest, SerializesToolResultSuccess) {
    const ContentBlock block = ToolResultBlock{
        .tool_use_id = "call_123",
        .content = "file.txt",
        .is_error = false,
    };
    const json j = content_block_to_json(block);

    EXPECT_EQ(j["type"], "tool_result");
    EXPECT_EQ(j["tool_use_id"], "call_123");
    EXPECT_EQ(j["content"], "file.txt");
    EXPECT_FALSE(j.contains("is_error"));
}

TEST(ContentBlockToJsonTest, SerializesToolResultError) {
    const ContentBlock block = ToolResultBlock{
        .tool_use_id = "call_456",
        .content = "not found",
        .is_error = true,
    };
    const json j = content_block_to_json(block);

    EXPECT_EQ(j["type"], "tool_result");
    EXPECT_TRUE(j["is_error"].get<bool>());
}

// ── message_to_json ─────────────────────────────

TEST(MessageToJsonTest, SerializesUserTextMessage) {
    const auto msg = Message::user_text("test input");
    const json j = message_to_json(msg);

    EXPECT_EQ(j["role"], "user");
    ASSERT_EQ(j["content"].size(), 1);
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "test input");
}

TEST(MessageToJsonTest, SerializesMultiBlockMessage) {
    const Message msg{
        .role = "assistant",
        .content =
            {
                TextBlock{.text = "thinking..."},
                ToolUseBlock{
                    .id = "id_1",
                    .name = "shell",
                    .input = {{"command", "pwd"}},
                },
            },
    };
    const json j = message_to_json(msg);

    EXPECT_EQ(j["role"], "assistant");
    ASSERT_EQ(j["content"].size(), 2);
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][1]["type"], "tool_use");
}
