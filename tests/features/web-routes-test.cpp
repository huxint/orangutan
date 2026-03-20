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

namespace {

orangutan::SessionMetadata make_session_metadata(std::string model, std::string scope_key, std::string agent_key, std::string origin_kind, std::string origin_ref) {
    return orangutan::SessionMetadata{
        .model = std::move(model),
        .scope_key = std::move(scope_key),
        .agent_key = std::move(agent_key),
        .origin_kind = std::move(origin_kind),
        .origin_ref = std::move(origin_ref),
    };
}

} // namespace

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
    cfg.model = "default-model";
    cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model", .subagents = {"helper"}};
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
    ASSERT_EQ(body.size(), 2);
    bool saw_default = false;
    bool saw_helper = false;
    for (const auto &agent : body) {
        if (agent["key"] == "default") {
            saw_default = true;
            EXPECT_EQ(agent["subagents"][0], "helper");
        }
        if (agent["key"] == "helper") {
            saw_helper = true;
        }
    }
    EXPECT_TRUE(saw_default);
    EXPECT_TRUE(saw_helper);
    server.stop();
}

TEST_F(WebRoutesTest, ListAgentSessionsReturnsOnlyMatchingAgentSessions) {
    orangutan::Config cfg;
    cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model"};
    cfg.agents["coder"] = orangutan::AgentConfig{.model = "coder-model"};

    session_store_->save({orangutan::Message::user_text("default")}, make_session_metadata("default-model", "agent:default|web", "default", "web", "web:local"));
    session_store_->save({orangutan::Message::user_text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));
    session_store_->save({orangutan::Message::user_text("channel")}, make_session_metadata("coder-model", "agent:coder|jid:qqbot:c2c:42", "coder", "channel", "qqbot:c2c:42"));

    orangutan::WebServer server;
    server.set_config(&cfg);
    server.set_session_store(session_store_.get());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Get("/api/agents/coder/sessions");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    ASSERT_TRUE(body.is_array());
    ASSERT_EQ(body.size(), 2);
    EXPECT_EQ(body[0]["agent_key"], "coder");
    EXPECT_EQ(body[1]["agent_key"], "coder");
    EXPECT_TRUE(body[0]["read_only"].get<bool>() || body[1]["read_only"].get<bool>());
    server.stop();
}

TEST_F(WebRoutesTest, GetAgentSessionRejectsCrossAgentAccess) {
    orangutan::Config cfg;
    cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model"};
    cfg.agents["coder"] = orangutan::AgentConfig{.model = "coder-model"};
    const auto coder_session_id =
        session_store_->save({orangutan::Message::user_text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));

    orangutan::WebServer server;
    server.set_config(&cfg);
    server.set_session_store(session_store_.get());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Get(("/api/agents/default/sessions/" + coder_session_id).c_str());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
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
