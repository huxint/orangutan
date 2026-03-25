#include "features/heartbeat/protocol/heartbeat-ok.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {

TEST_CASE("exact_match_suppresses") {
    CHECK(orangutan::detect_heartbeat_ok("HEARTBEAT_OK", 300));
};

TEST_CASE("prefix_with_short_text_suppresses") {
    std::string stripped;
    CHECK(orangutan::detect_heartbeat_ok("HEARTBEAT_OK All clear.", 300, &stripped));
    CHECK(stripped == "All clear.");
};

TEST_CASE("suffix_token_suppresses") {
    std::string stripped;
    CHECK(orangutan::detect_heartbeat_ok("Everything looks good.\nHEARTBEAT_OK", 300, &stripped));
    CHECK(stripped == "Everything looks good.");
};

TEST_CASE("prefix_with_long_text_does_not_suppress") {
    const auto long_text = std::string("HEARTBEAT_OK ") + std::string(400, 'x');
    CHECK_FALSE(orangutan::detect_heartbeat_ok(long_text, 300));
};

TEST_CASE("token_in_middle_does_not_suppress") {
    CHECK_FALSE(orangutan::detect_heartbeat_ok("Something HEARTBEAT_OK something", 300));
};

TEST_CASE("no_token_does_not_suppress") {
    CHECK_FALSE(orangutan::detect_heartbeat_ok("All systems operational", 300));
};

TEST_CASE("empty_response_does_not_suppress") {
    CHECK_FALSE(orangutan::detect_heartbeat_ok("", 300));
};

TEST_CASE("whitespace_around_token_suppresses") {
    CHECK(orangutan::detect_heartbeat_ok("  HEARTBEAT_OK  ", 300));
};

TEST_CASE("suppression_requires_heartbeat_jid") {
    CHECK(orangutan::should_suppress_heartbeat_reply("heartbeat:daily", "HEARTBEAT_OK", 300));
    CHECK_FALSE(orangutan::should_suppress_heartbeat_reply("cli", "HEARTBEAT_OK", 300));
};

} // namespace
