#include "providers/openai-protocol.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    using orangutan::providers::detail::finalize_openai_tool_call;
    using orangutan::providers::detail::merge_chat_completions_tool_call_delta;
    using orangutan::providers::detail::OpenAiToolCallState;
    using orangutan::providers::detail::serialize_openai_assistant_message;

    TEST_CASE("chat completions tool call keeps existing name when later delta is empty") {
        OpenAiToolCallState state;

        merge_chat_completions_tool_call_delta(state, nlohmann::json{{"index", 0}, {"id", "call_1"}, {"function", {{"name", "memory_list"}, {"arguments", "{\"limit\":20}"}}}});
        merge_chat_completions_tool_call_delta(state, nlohmann::json{{"index", 0}, {"function", {{"name", ""}, {"arguments", ""}}}});

        const auto tool_use = finalize_openai_tool_call(state, "test");
        REQUIRE(tool_use.has_value());
        CHECK(tool_use->id == "call_1");
        CHECK(tool_use->name == "memory_list");
        CHECK(tool_use->input["limit"] == 20);
    }

    TEST_CASE("chat completions tool call merges fragmented name and arguments") {
        OpenAiToolCallState state;

        merge_chat_completions_tool_call_delta(state, nlohmann::json{{"index", 0}, {"id", "call_2"}, {"function", {{"name", "memory_"}, {"arguments", "{\"limit\":"}}}});
        merge_chat_completions_tool_call_delta(state, nlohmann::json{{"index", 0}, {"function", {{"name", "list"}, {"arguments", "20}"}}}});

        const auto tool_use = finalize_openai_tool_call(state, "test");
        REQUIRE(tool_use.has_value());
        CHECK(tool_use->name == "memory_list");
        CHECK(tool_use->input["limit"] == 20);
    }

    TEST_CASE("assistant serialization skips malformed tool calls") {
        Message message(base::role::assistant, {Text{"working"}, ToolUse{"call_good", "memory_list", nlohmann::json{{"limit", 20}}}, ToolUse{"", "", nlohmann::json::object()}});

        const auto serialized = serialize_openai_assistant_message(message);
        REQUIRE(serialized.has_value());
        REQUIRE(serialized->contains("tool_calls"));
        REQUIRE((*serialized)["tool_calls"].size() == 1UL);
        CHECK((*serialized)["tool_calls"][0]["id"] == "call_good");
        CHECK((*serialized)["tool_calls"][0]["function"]["name"] == "memory_list");
        CHECK((*serialized)["content"] == "working");
    }

    TEST_CASE("assistant serialization drops message with only malformed tool calls") {
        Message message(base::role::assistant, {ToolUse{"", "", nlohmann::json::object()}});

        CHECK_FALSE(serialize_openai_assistant_message(message).has_value());
    }

} // namespace
