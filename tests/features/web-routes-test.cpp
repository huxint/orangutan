#include "features/web/web-server.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/config/config.hpp"
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>

class WebRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = "/tmp/orangutan-web-test-" + std::to_string(getpid()) + ".db";
        session_store_ = std::make_unique<orangutan::SessionStore>(db_path_);
    }
    void TearDown() override {
        session_store_.reset();
        std::filesystem::remove(db_path_);
    }
    std::string db_path_;
    std::unique_ptr<orangutan::SessionStore> session_store_;
};

TEST_F(WebRoutesTest, ListSessionsReturnsEmptyArray) {
    orangutan::WebServer server;
    server.set_session_store(session_store_.get());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/sessions");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_TRUE(body.is_array());
    EXPECT_TRUE(body.empty());
    server.stop();
}

TEST_F(WebRoutesTest, GetSessionReturns404ForMissing) {
    orangutan::WebServer server;
    server.set_session_store(session_store_.get());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/sessions/nonexistent");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    server.stop();
}

TEST_F(WebRoutesTest, DeleteSessionReturnsOk) {
    orangutan::WebServer server;
    server.set_session_store(session_store_.get());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Delete("/api/sessions/nonexistent");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    server.stop();
}

TEST_F(WebRoutesTest, GetConfigReturnsJson) {
    orangutan::Config cfg;
    cfg.model = "test-model";
    orangutan::WebServer server;
    server.set_config(&cfg);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/config");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["model"], "test-model");
    EXPECT_FALSE(body.contains("api_key"));
    server.stop();
}

TEST_F(WebRoutesTest, GetConfigReturns503WhenNull) {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/config");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    server.stop();
}

TEST_F(WebRoutesTest, PutConfigUpdatesModel) {
    orangutan::Config cfg;
    cfg.model = "old-model";
    orangutan::WebServer server;
    server.set_config(&cfg);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Put("/api/config", R"({"model":"new-model"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(cfg.model, "new-model");
    server.stop();
}

TEST_F(WebRoutesTest, ListToolsReturns503WhenNull) {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/tools");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    server.stop();
}

TEST_F(WebRoutesTest, ListAgentsReturnsArray) {
    orangutan::Config cfg;
    cfg.agents["helper"] = orangutan::AgentConfig{.model = "helper-model"};
    orangutan::WebServer server;
    server.set_config(&cfg);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/agents");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 1);
    EXPECT_EQ(body[0]["key"], "helper");
    server.stop();
}

TEST_F(WebRoutesTest, SystemStatusReturnsUptime) {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Get("/api/system/status");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_TRUE(body.contains("uptime_seconds"));
    EXPECT_TRUE(body.contains("active_web_sessions"));
    server.stop();
}
