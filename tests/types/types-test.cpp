#include "types/types.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <string>

using namespace orangutan;

TEST_CASE("message_text_builders_create_correct_roles") {
    struct RoleCase {
        base::role role;
        std::string text;
    };

    const std::array<RoleCase, 2> cases{{
        {.role = base::role::user, .text = "hello"},
        {.role = base::role::assistant, .text = "hi there"},
    }};

    for (const auto &test_case : cases) {
        const auto msg = test_case.role == base::role::user ? Message::user().text(test_case.text) : Message::assistant().text(test_case.text);

        CHECK(msg.role() == test_case.role);
        CHECK(std::distance(msg.begin(), msg.end()) == 1L);

        const auto *text = std::get_if<Text>(&*msg.begin());
        REQUIRE(text != nullptr);
        CHECK(text->text == test_case.text);
    }
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
    const Content block = ToolUse("call_123", "shell", {{"command", "ls"}});
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
    CHECK(j["content"].size() == 1UL);
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][0]["text"] == "test input");
};

TEST_CASE("message_to_json_serializes_multi_block_message") {
    const auto msg = Message(base::role::assistant, {Text{"thinking..."}, ToolUse("id_1", "shell", {{"command", "pwd"}})});
    const nlohmann::json j = message_to_json(msg);

    CHECK(j["role"] == "assistant");
    CHECK(j["content"].size() == 2UL);
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][1]["type"] == "tool_use");
};
