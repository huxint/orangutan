#include "channel/qq/qq-message-builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>

using namespace orangutan;

namespace {

    using TextSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
    using MarkdownSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
    using ReplySignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
    using ReferenceSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);

    static_assert(std::same_as<decltype(&QqMessageBuilder::text), TextSignature>);
    static_assert(std::same_as<decltype(&QqMessageBuilder::markdown), MarkdownSignature>);
    static_assert(std::same_as<decltype(&QqMessageBuilder::reply_to), ReplySignature>);
    static_assert(std::same_as<decltype(&QqMessageBuilder::reference), ReferenceSignature>);

} // namespace

TEST_CASE("qq_message_builder_constructs_text_payload_with_reply_and_reference") {
    const auto payload = QqMessageBuilder{}.text("hello").msg_seq(42).reply_to("msg-1").reference("msg-1").build();

    CHECK(payload.at("msg_type").get<int>() == 0);
    CHECK(payload.at("content").get<std::string>() == "hello");
    CHECK(payload.at("msg_seq").get<int>() == 42);
    CHECK(payload.at("msg_id").get<std::string>() == "msg-1");
    CHECK(payload.at("message_reference").at("message_id").get<std::string>() == "msg-1");
    CHECK(payload.at("message_reference").at("ignore_get_message_error").get<bool>());
}

TEST_CASE("qq_message_builder_constructs_markdown_payload") {
    const auto payload = QqMessageBuilder{}.markdown("# title").msg_seq(7).build();

    CHECK(payload.at("msg_type").get<int>() == 2);
    CHECK(payload.at("markdown").at("content").get<std::string>() == "# title");
    CHECK(payload.at("msg_seq").get<int>() == 7);
    CHECK_FALSE(payload.contains("content"));
}

TEST_CASE("qq_message_builder_constructs_media_payload") {
    const auto payload = QqMessageBuilder{}.media("file-info-1").msg_seq(3).build();

    CHECK(payload.at("msg_type").get<int>() == 7);
    CHECK(payload.at("media").at("file_info").get<std::string>() == "file-info-1");
    CHECK(payload.at("msg_seq").get<int>() == 3);
    CHECK_FALSE(payload.contains("content"));
    CHECK_FALSE(payload.contains("markdown"));
}

TEST_CASE("qq_message_builder_constructs_media_payload_with_caption") {
    const auto payload = QqMessageBuilder{}.media("file-info-1", "caption").msg_seq(4).build();

    CHECK(payload.at("msg_type").get<int>() == 7);
    CHECK(payload.at("media").at("file_info").get<std::string>() == "file-info-1");
    CHECK(payload.at("content").get<std::string>() == "caption");
    CHECK(payload.at("msg_seq").get<int>() == 4);
}

TEST_CASE("qq_message_builder_constructs_ark_payload") {
    const auto ark = nlohmann::json{
        {"template_id", 23},
        {"kv", nlohmann::json::array()},
    };

    const auto payload = QqMessageBuilder{}.ark(ark).msg_seq(5).build();

    CHECK(payload.at("msg_type").get<int>() == 3);
    CHECK(payload.at("ark") == ark);
    CHECK(payload.at("msg_seq").get<int>() == 5);
    CHECK_FALSE(payload.contains("content"));
    CHECK_FALSE(payload.contains("markdown"));
    CHECK_FALSE(payload.contains("media"));
}

TEST_CASE("qq_message_builder_constructs_embed_payload") {
    const auto embed = nlohmann::json{
        {"title", "hello"},
        {"prompt", "world"},
    };

    const auto payload = QqMessageBuilder{}.embed(embed).msg_seq(6).build();

    CHECK(payload.at("msg_type").get<int>() == 4);
    CHECK(payload.at("embed") == embed);
    CHECK(payload.at("msg_seq").get<int>() == 6);
    CHECK_FALSE(payload.contains("content"));
    CHECK_FALSE(payload.contains("markdown"));
    CHECK_FALSE(payload.contains("media"));
}

TEST_CASE("qq_message_builder_attaches_keyboard_payload") {
    const auto keyboard = nlohmann::json{
        {"content", {{"rows", nlohmann::json::array()}}},
    };

    const auto payload = QqMessageBuilder{}.markdown("pick one").keyboard(keyboard).msg_seq(8).build();

    CHECK(payload.at("msg_type").get<int>() == 2);
    CHECK(payload.at("keyboard") == keyboard);
}

TEST_CASE("qq_message_builder_clears_reply_and_reference_when_empty") {
    auto builder = QqMessageBuilder{};
    const auto payload = builder.text("hello").reply_to("msg-1").reference("msg-2").reply_to("").reference("").build();

    CHECK_FALSE(payload.contains("msg_id"));
    CHECK_FALSE(payload.contains("message_reference"));
}
