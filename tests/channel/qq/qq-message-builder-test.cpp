#include "channel/qq/qq-message-builder.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

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
