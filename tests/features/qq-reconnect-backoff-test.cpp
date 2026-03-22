#include "features/channel/platforms/qq-reconnect-backoff.hpp"

#include <chrono>
#include <gtest/gtest.h>

using namespace orangutan;

TEST(QqReconnectBackoffTest, UsesCappedExponentialBackoff) {
    QqReconnectBackoff backoff;

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(2));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(4));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(8));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(15));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(15));
}

TEST(QqReconnectBackoffTest, ResetRestartsSequence) {
    QqReconnectBackoff backoff;

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(2));

    backoff.reset();

    EXPECT_EQ(backoff.next_delay(), std::chrono::seconds(1));
}
