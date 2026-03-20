#include "features/web/web-server.hpp"
#include "features/web/web-routes.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "infra/config/config.hpp"
#include "test-helpers.hpp"
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <memory>

using orangutan::testing::ScopedEnvVar;
using orangutan::testing::test_tmp_root;

class WebRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = test_tmp_root() / ("web-routes-" + std::to_string(getpid()));
        home_root_ = temp_root_ / "home";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(home_root_ / ".orangutan");
        home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_root_.string());
        db_path_ = "/tmp/orangutan-web-test-" + std::to_string(getpid()) + ".db";
        session_store_ = std::make_unique<orangutan::SessionStore>(db_path_);
    }
    void TearDown() override {
        session_store_.reset();
        std::filesystem::remove(db_path_);
        home_env_.reset();
        std::filesystem::remove_all(temp_root_);
    }
    std::filesystem::path temp_root_;
    std::filesystem::path home_root_;
    std::filesystem::path config_path() const {
        return home_root_ / ".orangutan" / "config.toml";
    }
    std::string db_path_;
    std::unique_ptr<orangutan::SessionStore> session_store_;
    std::unique_ptr<ScopedEnvVar> home_env_;
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

bool has_tool_named(const std::vector<orangutan::ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&name](const orangutan::ToolDef &definition) {
        return definition.name == name;
    });
}

orangutan::Config make_runtime_config(const std::filesystem::path &workspace_root) {
    orangutan::Config cfg;
    cfg.provider = "openai";
    cfg.model = "gpt-test";
    cfg.base_url = "https://example.test";
    cfg.api_key = "test-key";
    cfg.custom_tools.push_back(orangutan::Config::ScriptToolConfig{
        .name = "custom_echo",
        .description = "Custom echo tool",
        .command = "echo hello",
    });
    cfg.agents["default"] = orangutan::AgentConfig{
        .provider = "openai",
        .model = "gpt-test",
        .base_url = "https://example.test",
        .api_key = "test-key",
        .system_prompt = "You are a web runtime test agent.",
        .workspace = workspace_root.string(),
        .permissions =
            {
                .sandbox_mode = orangutan::ToolSandboxMode::isolated,
                .shell_approval = orangutan::ToolApprovalPolicy::ask,
            },
        .subagents = {"coder"},
    };
    cfg.agents["coder"] = orangutan::AgentConfig{
        .provider = "openai",
        .model = "gpt-coder-test",
        .base_url = "https://example.test",
        .api_key = "test-key",
        .system_prompt = "You are coder.",
        .workspace = workspace_root.string(),
    };
    return cfg;
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
    server.set_config_save_path(config_path().string());
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Put("/api/config", R"({"model":"new-model"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(cfg.model, "new-model");
    ASSERT_TRUE(std::filesystem::exists(config_path()));
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

TEST_F(WebRoutesTest, SharedWebRuntimeBuildsWithParityContextAndTools) {
    const auto workspace = temp_root_ / "runtime-workspace";
    std::filesystem::create_directories(workspace);

    auto cfg = make_runtime_config(workspace);
    orangutan::MemoryStore memory_store((temp_root_ / "memory.db").string());
    orangutan::SubagentRunStore run_store((temp_root_ / "subagent-runs.db").string());
    orangutan::SubagentManager subagent_manager(run_store, [](const orangutan::SubagentWorkerRequest &) {
        return orangutan::SubagentWorkerResult{.status = orangutan::SubagentRunStatus::succeeded};
    });
    std::string session_id = "web-session";

    auto runtime =
        orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &subagent_manager, [](const orangutan::ToolUseBlock &, const std::string &) {
            return false;
        });

    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "memory_list"));
    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "shell"));
    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "custom_echo"));
    EXPECT_EQ(runtime.tool_context.runtime_origin, orangutan::SubagentRuntimeOrigin::web);
    EXPECT_EQ(runtime.tool_context.raw_caller_id, "web:local");
    EXPECT_EQ(runtime.tool_context.current_session_id, &session_id);
    EXPECT_EQ(runtime.tool_context.allowed_child_agents, std::vector<std::string>({"coder"}));
    EXPECT_TRUE(static_cast<bool>(runtime.tool_context.approval_callback));
    EXPECT_TRUE(runtime.agent != nullptr);

    const auto shell_result = runtime.tools.execute(orangutan::ToolUseBlock{
        .id = "web-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(shell_result.is_error);
    EXPECT_TRUE(shell_result.content.find("requires approval") != std::string::npos || shell_result.content.find("rejected by user") != std::string::npos);
}

TEST_F(WebRoutesTest, SharedWebRuntimeLoadsSkillsAndHooks) {
    const auto workspace = temp_root_ / "skills-hooks-workspace";
    const auto skill_root = workspace / "skills";
    const auto hook_root = workspace / "hooks";
    std::filesystem::create_directories(skill_root / "web-runtime-skill");
    std::filesystem::create_directories(hook_root / "before_tool_call");

    {
        std::ofstream out(skill_root / "web-runtime-skill" / "SKILL.md");
        out << "+++\n";
        out << "name = \"web-runtime-skill\"\n";
        out << "description = \"web runtime skill\"\n";
        out << "+++\n\n";
        out << "Use this skill for web runtime checks.\n";
    }
    {
        const auto hook = hook_root / "before_tool_call" / "01-web-hook.sh";
        std::ofstream out(hook);
        out << "#!/bin/sh\n";
        out << "exit 0\n";
        out.close();
        std::filesystem::permissions(hook, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    auto cfg = make_runtime_config(workspace);
    cfg.skill_paths = {skill_root.string()};
    cfg.hook_paths = {hook_root.string()};
    orangutan::MemoryStore memory_store((temp_root_ / "memory-skills.db").string());
    orangutan::SubagentRunStore run_store((temp_root_ / "subagent-runs-skills.db").string());
    orangutan::SubagentManager subagent_manager(run_store, [](const orangutan::SubagentWorkerRequest &) {
        return orangutan::SubagentWorkerResult{.status = orangutan::SubagentRunStatus::succeeded};
    });
    std::string session_id = "web-session-skills";

    auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &subagent_manager);

    EXPECT_NE(runtime.skills_prompt.find("web-runtime-skill"), std::string::npos);
    ASSERT_NE(runtime.hook_manager, nullptr);
    EXPECT_EQ(runtime.hook_manager->hook_count(orangutan::HookEvent::before_tool_call), 1);
}
