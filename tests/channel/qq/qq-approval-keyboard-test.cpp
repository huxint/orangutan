#include "channel/qq/qq-approval-keyboard.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

TEST_CASE("qq_approval_keyboard_builds_callback_buttons_when_always_allow_is_enabled") {
    const auto keyboard = channel::qq::build_approval_keyboard("tool-approval-1", true);
    const auto &buttons = keyboard.at("content").at("rows").at(0).at("buttons");

    REQUIRE(buttons.size() == 3UL);
    CHECK(buttons.at(0).at("render_data").at("label").get<std::string>() == "once");
    CHECK(buttons.at(1).at("render_data").at("label").get<std::string>() == "always");
    CHECK(buttons.at(2).at("render_data").at("label").get<std::string>() == "deny");
    CHECK(buttons.at(0).at("action").at("type").get<int>() == 1);
    CHECK(buttons.at(0).at("action").at("click_limit").get<int>() == 1);
    CHECK(buttons.at(0).at("group_id").get<std::string>() == "approval");

    const auto parsed = channel::qq::parse_approval_callback_data(buttons.at(1).at("action").at("data").get<std::string>());
    REQUIRE(parsed.has_value());
    CHECK(parsed->request_id == "tool-approval-1");
    CHECK(parsed->action == channel::qq::approval_action::always_allow);
}

TEST_CASE("qq_approval_keyboard_omits_always_allow_button_when_disabled") {
    const auto keyboard = channel::qq::build_approval_keyboard("tool-approval-2", false);
    const auto &buttons = keyboard.at("content").at("rows").at(0).at("buttons");

    REQUIRE(buttons.size() == 2UL);
    CHECK(buttons.at(0).at("render_data").at("label").get<std::string>() == "once");
    CHECK(buttons.at(1).at("render_data").at("label").get<std::string>() == "deny");

    const auto allow_once = channel::qq::parse_approval_callback_data(buttons.at(0).at("action").at("data").get<std::string>());
    REQUIRE(allow_once.has_value());
    CHECK(allow_once->request_id == "tool-approval-2");
    CHECK(allow_once->action == channel::qq::approval_action::allow_once);

    const auto deny = channel::qq::parse_approval_callback_data(buttons.at(1).at("action").at("data").get<std::string>());
    REQUIRE(deny.has_value());
    CHECK(deny->request_id == "tool-approval-2");
    CHECK(deny->action == channel::qq::approval_action::deny);
}

TEST_CASE("qq_approval_callback_parser_rejects_malformed_payloads") {
    CHECK_FALSE(channel::qq::parse_approval_callback_data("approval::allow_once").has_value());
    CHECK_FALSE(channel::qq::parse_approval_callback_data("approval:tool-approval-1:maybe").has_value());
    CHECK_FALSE(channel::qq::parse_approval_callback_data("tool-approval-1 yes").has_value());
}
