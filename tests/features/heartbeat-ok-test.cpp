#include "features/heartbeat/protocol/heartbeat-ok.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace orangutan;

TEST(HeartbeatOkTest, ExactMatchSuppresses) {
    EXPECT_TRUE(detect_heartbeat_ok("HEARTBEAT_OK", 300));
}

TEST(HeartbeatOkTest, PrefixWithShortTextSuppresses) {
    std::string stripped;
    EXPECT_TRUE(detect_heartbeat_ok("HEARTBEAT_OK All clear.", 300, &stripped));
    EXPECT_EQ(stripped, "All clear.");
}

TEST(HeartbeatOkTest, SuffixTokenSuppresses) {
    std::string stripped;
    EXPECT_TRUE(detect_heartbeat_ok("Everything looks good.\nHEARTBEAT_OK", 300, &stripped));
    EXPECT_EQ(stripped, "Everything looks good.");
}

TEST(HeartbeatOkTest, PrefixWithLongTextDoesNotSuppress) {
    auto long_text = std::string("HEARTBEAT_OK ") + std::string(400, 'x');
    EXPECT_FALSE(detect_heartbeat_ok(long_text, 300));
}

TEST(HeartbeatOkTest, TokenInMiddleDoesNotSuppress) {
    EXPECT_FALSE(detect_heartbeat_ok("Something HEARTBEAT_OK something", 300));
}

TEST(HeartbeatOkTest, NoTokenDoesNotSuppress) {
    EXPECT_FALSE(detect_heartbeat_ok("All systems operational", 300));
}

TEST(HeartbeatOkTest, EmptyResponseDoesNotSuppress) {
    EXPECT_FALSE(detect_heartbeat_ok("", 300));
}

TEST(HeartbeatOkTest, WhitespaceAroundTokenSuppresses) {
    EXPECT_TRUE(detect_heartbeat_ok("  HEARTBEAT_OK  ", 300));
}

TEST(HeartbeatOkTest, SuppressionRequiresHeartbeatJid) {
    EXPECT_TRUE(should_suppress_heartbeat_reply("heartbeat:daily", "HEARTBEAT_OK", 300));
    EXPECT_FALSE(should_suppress_heartbeat_reply("cli", "HEARTBEAT_OK", 300));
}
