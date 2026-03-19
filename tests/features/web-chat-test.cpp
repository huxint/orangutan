#include "features/web/web-server.hpp"
#include "infra/config/config.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace orangutan {

TEST(WebChatTest, ChatEndpointRejectsMissingMessage) {
    WebServer server;
    Config config;
    config.provider = "anthropic";
    config.model = "test";
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    server.stop();
}

TEST(WebChatTest, ChatEndpointRejectsInvalidJson) {
    WebServer server;
    Config config;
    config.provider = "anthropic";
    config.model = "test";
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", "not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    server.stop();
}

TEST(WebChatTest, ChatEndpointReturns503WithoutConfig) {
    WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"hello"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);

    server.stop();
}

TEST(WebChatTest, AbortEndpointReturns404ForUnknownSession) {
    WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat/abort", R"({"session_id":"nonexistent"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    server.stop();
}

TEST(WebChatTest, AbortEndpointRejectsMissingSessionId) {
    WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat/abort", "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    server.stop();
}

} // namespace orangutan
