#include "features/web/web-server.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace orangutan {

namespace {

SessionMetadata make_session_metadata(std::string model, std::string scope_key, std::string agent_key, std::string origin_kind, std::string origin_ref) {
    return SessionMetadata{
        .model = std::move(model),
        .scope_key = std::move(scope_key),
        .agent_key = std::move(agent_key),
        .origin_kind = std::move(origin_kind),
        .origin_ref = std::move(origin_ref),
    };
}

Config make_config() {
    Config config;
    config.provider = "openai";
    config.model = "test";
    config.base_url = "https://example.test";
    config.api_key = "test-key";
    config.agents["default"] = AgentConfig{
        .provider = "openai",
        .model = "test",
        .base_url = "https://example.test",
        .api_key = "test-key",
        .system_prompt = "You are a test agent.",
    };
    config.agents["coder"] = AgentConfig{
        .provider = "openai",
        .model = "coder-test",
        .base_url = "https://example.test",
        .api_key = "test-key",
        .system_prompt = "You are a coder agent.",
    };
    return config;
}

class WebChatStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / ("orangutan-web-chat-test-" + std::to_string(getpid()) + ".db");
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    std::filesystem::path db_path_;
};

} // namespace

TEST(WebChatTest, ChatEndpointRejectsMissingMessage) {
    WebServer server;
    Config config = make_config();
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
    Config config = make_config();
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

    auto res = cli.Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);

    server.stop();
}

TEST(WebChatTest, ChatEndpointRejectsMissingAgentKey) {
    WebServer server;
    Config config = make_config();
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"hello"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    server.stop();
}

TEST(WebChatTest, ChatEndpointRejectsMissingApiKey) {
    orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
    orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

    WebServer server;
    Config config = make_config();
    config.api_key.clear();
    config.agents["default"].api_key.clear();
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "missing API key for agent 'default'");

    server.stop();
}

TEST(WebChatTest, ChatEndpointRejectsInvalidWorkspaceConfig) {
    const auto workspace_file = orangutan::testing::test_tmp_root() / "web-chat-invalid-workspace";
    std::filesystem::remove(workspace_file);
    {
        std::ofstream out(workspace_file);
        out << "not a directory\n";
    }

    WebServer server;
    Config config = make_config();
    config.agents["default"].workspace = workspace_file.string();
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_NE(body["error"].get<std::string>().find("failed to resolve workspace for agent 'default'"), std::string::npos);

    server.stop();
    std::filesystem::remove(workspace_file);
}

TEST_F(WebChatStoreTest, ChatEndpointRejectsReadOnlyChannelSession) {
    SessionStore store(db_path_.string());
    Config config = make_config();
    const auto session_id = store.save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));

    WebServer server;
    server.set_config(&config);
    server.set_session_store(&store);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat",
                        (nlohmann::json{
                             {"message", "hello again"},
                             {"agent_key", "default"},
                             {"session_id", session_id},
                         })
                            .dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409);

    server.stop();
}

TEST_F(WebChatStoreTest, ChatEndpointRejectsCrossAgentSessionAccess) {
    SessionStore store(db_path_.string());
    Config config = make_config();
    const auto session_id = store.save({Message::user_text("hello")}, make_session_metadata("coder-test", "agent:coder|web", "coder", "web", "web:local"));

    WebServer server;
    server.set_config(&config);
    server.set_session_store(&store);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat",
                        (nlohmann::json{
                             {"message", "hello again"},
                             {"agent_key", "default"},
                             {"session_id", session_id},
                         })
                            .dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

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
