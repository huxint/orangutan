#include "core/types.hpp"

#include "support/ut.hpp"

using namespace orangutan;

boost::ut::suite types_test_suite = [] {
    using namespace boost::ut;

    "message_user_text_creates_correct_role"_test = [] {
        const auto msg = Message::user_text("hello");

        expect(msg.role == Role::User);
        expect(msg.content.size() == 1_ul);

        const auto *text = std::get_if<TextBlock>(&msg.content[0]);
        expect((text != nullptr) >> fatal);
        expect(text->text == "hello");
    };

    "message_assistant_text_creates_correct_role"_test = [] {
        const auto msg = Message::assistant_text("hi there");

        expect(msg.role == Role::Assistant);
        expect(msg.content.size() == 1_ul);

        const auto *text = std::get_if<TextBlock>(&msg.content[0]);
        expect((text != nullptr) >> fatal);
        expect(text->text == "hi there");
    };

    "message_empty_text_is_allowed"_test = [] {
        const auto msg = Message::user_text("");

        const auto *text = std::get_if<TextBlock>(&msg.content[0]);
        expect((text != nullptr) >> fatal);
        expect(text->text.empty());
    };

    "content_block_to_json_serializes_text_block"_test = [] {
        const ContentBlock block = TextBlock{"hello world"};
        const json j = content_block_to_json(block);

        expect(j["type"] == "text");
        expect(j["text"] == "hello world");
    };

    "content_block_to_json_serializes_tool_use_block"_test = [] {
        const ContentBlock block = ToolUseBlock{
            .id = "call_123",
            .name = "shell",
            .input = {{"command", "ls"}},
        };
        const json j = content_block_to_json(block);

        expect(j["type"] == "tool_use");
        expect(j["id"] == "call_123");
        expect(j["name"] == "shell");
        expect(j["input"]["command"] == "ls");
    };

    "content_block_to_json_serializes_tool_result_success"_test = [] {
        const ContentBlock block = ToolResultBlock{
            .tool_use_id = "call_123",
            .content = "file.txt",
            .is_error = false,
        };
        const json j = content_block_to_json(block);

        expect(j["type"] == "tool_result");
        expect(j["tool_use_id"] == "call_123");
        expect(j["content"] == "file.txt");
        expect(not j.contains("is_error"));
    };

    "content_block_to_json_serializes_tool_result_error"_test = [] {
        const ContentBlock block = ToolResultBlock{
            .tool_use_id = "call_456",
            .content = "not found",
            .is_error = true,
        };
        const json j = content_block_to_json(block);

        expect(j["type"] == "tool_result");
        expect(j["is_error"].get<bool>());
    };

    "message_to_json_serializes_user_text_message"_test = [] {
        const auto msg = Message::user_text("test input");
        const json j = message_to_json(msg);

        expect(j["role"] == "user");
        expect(j["content"].size() == 1_ul);
        expect(j["content"][0]["type"] == "text");
        expect(j["content"][0]["text"] == "test input");
    };

    "message_to_json_serializes_multi_block_message"_test = [] {
        const Message msg{
            .role = Role::Assistant,
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

        expect(j["role"] == "assistant");
        expect(j["content"].size() == 2_ul);
        expect(j["content"][0]["type"] == "text");
        expect(j["content"][1]["type"] == "tool_use");
    };
};
