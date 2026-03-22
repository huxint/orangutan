#include "features/channel/qq/reconnect-backoff.hpp"

#include <chrono>
#include <gtest/gtest.h>

using namespace orangutan;
using namespace orangutan::qq;

TEST(ReconnectBackoffTest, UsesCappedExponentialBackoff) {
    ReconnectBackoff backoff;

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(2));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(4));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(8));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(15));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(15));
}

TEST(ReconnectBackoffTest, ResetRestartsSequence) {
    ReconnectBackoff backoff;

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(2));

    backoff.reset();

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
}
