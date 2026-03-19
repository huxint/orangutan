#include "features/web/web-server.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

TEST(WebServerTest, StartsAndStopsCleanly) {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0); // port 0 = OS picks free port
    EXPECT_GT(server.port(), 0);
    EXPECT_TRUE(server.is_running());
    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(WebServerTest, ServesHealthEndpoint) {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    server.stop();
}
