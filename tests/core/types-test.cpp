#include "core/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <iterator>

using namespace orangutan;

TEST_CASE("message_user_text_creates_correct_role") {
    const auto msg = Message::user().text("hello");

    CHECK(msg.role() == base::role::user);
    CHECK(std::distance(msg.begin(), msg.end()) == 1l);

    const auto *text = std::get_if<Text>(&*msg.begin());
    REQUIRE(text != nullptr);
    CHECK(text->text == "hello");
};

TEST_CASE("message_assistant_text_creates_correct_role") {
    const auto msg = Message::assistant().text("hi there");

    CHECK(msg.role() == base::role::assistant);
    CHECK(std::distance(msg.begin(), msg.end()) == 1l);

    const auto *text = std::get_if<Text>(&*msg.begin());
    REQUIRE(text != nullptr);
    CHECK(text->text == "hi there");
};

TEST_CASE("message_empty_text_is_allowed") {
    const auto msg = Message::user().text("");

    const auto *text = std::get_if<Text>(&*msg.begin());
    REQUIRE(text != nullptr);
    CHECK(text->text.empty());
};

TEST_CASE("content_block_to_json_serializes_text_block") {
    const Content block = Text{"hello world"};
    const nlohmann::json j = content_block_to_json(block);

    CHECK(j["type"] == "text");
    CHECK(j["text"] == "hello world");
};

TEST_CASE("content_block_to_json_serializes_tool_use_block") {
    const Content block = ToolUse{
        .id = "call_123",
        .name = "shell",
        .input = {{"command", "ls"}},
    };
    const nlohmann::json j = content_block_to_json(block);

    CHECK(j["type"] == "tool_use");
    CHECK(j["id"] == "call_123");
    CHECK(j["name"] == "shell");
    CHECK(j["input"]["command"] == "ls");
};

TEST_CASE("content_block_to_json_serializes_tool_result_success") {
    const Content block = ToolResult{"call_123", "file.txt", false};
    const nlohmann::json j = content_block_to_json(block);

    CHECK(j["type"] == "tool_result");
    CHECK(j["tool_use_id"] == "call_123");
    CHECK(j["content"] == "file.txt");
    CHECK_FALSE(j.contains("is_error"));
};

TEST_CASE("content_block_to_json_serializes_tool_result_error") {
    const Content block = ToolResult{"call_456", "not found", true};
    const nlohmann::json j = content_block_to_json(block);

    CHECK(j["type"] == "tool_result");
    CHECK(j["is_error"].get<bool>());
};

TEST_CASE("message_to_json_serializes_user_text_message") {
    const auto msg = Message::user().text("test input");
    const nlohmann::json j = message_to_json(msg);

    CHECK(j["role"] == "user");
    CHECK(j["content"].size() == 1ul);
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][0]["text"] == "test input");
};

TEST_CASE("message_to_json_serializes_multi_block_message") {
    const auto msg = Message(base::role::assistant, {
                                                        Text{"thinking..."},
                                                        ToolUse{
                                                            .id = "id_1",
                                                            .name = "shell",
                                                            .input = {{"command", "pwd"}},
                                                        },
                                                    });
    const nlohmann::json j = message_to_json(msg);

    CHECK(j["role"] == "assistant");
    CHECK(j["content"].size() == 2ul);
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][1]["type"] == "tool_use");
};
