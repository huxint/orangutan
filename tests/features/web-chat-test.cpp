#include "features/web/web-server.hpp"
#include "features/web/web-routes.hpp"
#include "features/agent/agent-loop.hpp"
#include "features/automation/runtime.hpp"
#include "features/automation/store.hpp"
#include "features/tools/core/background-completion.hpp"
#include "infra/config/config.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

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

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&name](const ToolDef &definition) {
        return definition.name == name;
    });
}

const automation::InboxItem *find_inbox_item_by_body_type(const std::vector<automation::InboxItem> &items, const std::string &type) {
    const auto it = std::ranges::find_if(items, [&](const automation::InboxItem &item) {
        return json::parse(item.body).value("type", "") == type;
    });
    return it == items.end() ? nullptr : &(*it);
}

std::optional<json> find_sse_event_payload(std::string_view body, std::string_view event_name) {
    const auto marker = "event: " + std::string(event_name) + "\ndata: ";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }

    const auto payload_start = start + marker.size();
    const auto payload_end = body.find("\n\n", payload_start);
    if (payload_end == std::string_view::npos) {
        return std::nullopt;
    }

    return json::parse(std::string(body.substr(payload_start, payload_end - payload_start)));
}

class ScriptedProvider final : public Provider {
public:
    using Step = std::function<LLMResponse(const std::vector<Message> &)>;

    explicit ScriptedProvider(std::vector<Step> steps)
    : steps_(std::move(steps)) {}

    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &messages, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        if (next_step_ >= steps_.size()) {
            throw std::runtime_error("no scripted response available");
        }
        return steps_[next_step_++](messages);
    }

    std::string name() const override {
        return "scripted-provider";
    }

private:
    std::vector<Step> steps_;
    size_t next_step_ = 0;
};

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

TEST(WebChatTest, HelpSlashCommandStreamsHelpWithoutApiKey) {
    orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
    orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

    WebServer server;
    Config config = make_config();
    config.api_key.clear();
    config.agents["default"].api_key.clear();
    server.set_config(&config);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"/help","agent_key":"default"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    ASSERT_TRUE(find_sse_event_payload(res->body, "text").has_value());
    EXPECT_NE(find_sse_event_payload(res->body, "text")->at("text").get<std::string>().find("/tasks run <id>"), std::string::npos);

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

TEST_F(WebChatStoreTest, ResumeSlashCommandStreamsTargetSessionEvent) {
    SessionStore store(db_path_.string());
    Config config = make_config();
    config.api_key.clear();
    config.agents["default"].api_key.clear();

    const auto session_id = store.save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));

    WebServer server;
    server.set_config(&config);
    server.set_session_store(&store);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    const auto req = json{
        {"message", std::string("/resume ") + session_id},
        {"agent_key", "default"},
    };
    auto res = cli.Post("/api/chat", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    ASSERT_TRUE(find_sse_event_payload(res->body, "session").has_value());
    EXPECT_EQ(find_sse_event_payload(res->body, "session")->at("session_id"), session_id);
    ASSERT_TRUE(find_sse_event_payload(res->body, "text").has_value());
    EXPECT_NE(find_sse_event_payload(res->body, "text")->at("text").get<std::string>().find(session_id), std::string::npos);

    server.stop();
}

TEST_F(WebChatStoreTest, NewSlashCommandStreamsMarkdownReplyAndSessionEvent) {
    SessionStore store(db_path_.string());
    Config config = make_config();
    config.api_key.clear();
    config.agents["default"].api_key.clear();

    const auto existing_session_id = store.save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));

    WebServer server;
    server.set_config(&config);
    server.set_session_store(&store);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    const auto req = json{
        {"message", "/new"},
        {"agent_key", "default"},
        {"session_id", existing_session_id},
    };
    auto res = cli.Post("/api/chat", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    ASSERT_TRUE(find_sse_event_payload(res->body, "session").has_value());
    EXPECT_NE(find_sse_event_payload(res->body, "session")->at("session_id"), existing_session_id);
    EXPECT_FALSE(find_sse_event_payload(res->body, "text").has_value());

    server.stop();
}

TEST_F(WebChatStoreTest, ExportSlashCommandWorksForReadOnlyChannelSession) {
    SessionStore store(db_path_.string());
    Config config = make_config();
    config.api_key.clear();
    config.agents["default"].api_key.clear();
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_web_export_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    config.agents["default"].workspace = workspace.string();

    const auto session_id = store.save({Message::user_text("hello"), Message::assistant_text("copied reply")},
                                       make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
    const auto export_path = workspace / ".exports" / (session_id + ".md");

    WebServer server;
    server.set_config(&config);
    server.set_session_store(&store);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    const auto req = json{
        {"message", "/export"},
        {"agent_key", "default"},
        {"session_id", session_id},
    };
    auto res = cli.Post("/api/chat", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    ASSERT_TRUE(find_sse_event_payload(res->body, "text").has_value());
    EXPECT_EQ(find_sse_event_payload(res->body, "text")->at("text"), "## Export\n- Saved current session to `" + export_path.string() + '`');
    ASSERT_TRUE(std::filesystem::exists(export_path));

    std::ifstream in(export_path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("# Session Export"), std::string::npos);
    EXPECT_NE(content.find("hello"), std::string::npos);
    EXPECT_NE(content.find("copied reply"), std::string::npos);

    server.stop();
    std::filesystem::remove_all(workspace);
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

TEST(WebChatTest, RuntimeBundleIncludesCustomAndMemoryTools) {
    const auto workspace = orangutan::testing::test_tmp_root() / "web-chat-runtime-workspace";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    auto config = make_config();
    config.custom_tools.push_back(Config::ScriptToolConfig{
        .name = "custom_echo",
        .description = "Custom echo script tool",
        .command = "echo hello",
    });
    config.agents["default"].workspace = workspace.string();
    config.agents["default"].subagents = {"coder"};
    config.agents["default"].permissions = {
        .sandbox_mode = ToolSandboxMode::isolated,
        .shell_approval = ToolApprovalPolicy::ask,
    };
    config.agents["coder"].workspace = workspace.string();

    MemoryStore memory_store((workspace / "memory.db").string());
    SubagentRunStore run_store((workspace / "runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    std::string session_id = "web-chat-runtime-session";

    auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, &subagent_manager, nullptr, [](const ToolUseBlock &, const std::string &) {
        return false;
    });

    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "memory_list"));
    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "custom_echo"));
    EXPECT_EQ(runtime.tool_context.runtime_origin, SubagentRuntimeOrigin::web);
    EXPECT_EQ(runtime.tool_context.raw_caller_id, "web:local");
    EXPECT_EQ(runtime.tool_context.current_session_id, &session_id);
    EXPECT_EQ(runtime.tool_context.allowed_child_agents, std::vector<std::string>({"coder"}));
    EXPECT_TRUE(static_cast<bool>(runtime.tool_context.approval_callback));
    EXPECT_TRUE(runtime.agent != nullptr);

    const auto shell_result = runtime.tools.execute(ToolUseBlock{
        .id = "web-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(shell_result.is_error);
    EXPECT_TRUE(shell_result.content.find("requires approval") != std::string::npos || shell_result.content.find("rejected by user") != std::string::npos);
}

TEST(WebChatTest, TasksSlashCommandUsesRuntimeToolOutput) {
    WebServer server;
    Config config = make_config();
    auto automation_store =
        std::make_shared<automation::Store>((std::filesystem::temp_directory_path() / ("orangutan-web-chat-tasks-" + std::to_string(getpid()) + ".db")).string());
    automation::Runtime automation_runtime(*automation_store);
    server.set_config(&config);
    server.set_automation_runtime(&automation_runtime);
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());

    auto res = cli.Post("/api/chat", R"({"message":"/tasks","agent_key":"default"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    ASSERT_TRUE(find_sse_event_payload(res->body, "text").has_value());
    EXPECT_EQ(find_sse_event_payload(res->body, "text")->at("text"), "## Tasks\n- 🗓️ No tasks configured.");

    server.stop();
}

TEST(WebChatTest, RuntimeBundleLoadsSkillsAndHooksFromConfiguredPaths) {
    const auto workspace = orangutan::testing::test_tmp_root() / "web-chat-runtime-skills-hooks";
    const auto skill_root = workspace / "skills";
    const auto hook_root = workspace / "hooks";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(skill_root / "web-chat-runtime-skill");
    std::filesystem::create_directories(hook_root / "before_tool_call");

    {
        std::ofstream out(skill_root / "web-chat-runtime-skill" / "SKILL.md");
        out << "+++\n";
        out << "name = \"web-chat-runtime-skill\"\n";
        out << "description = \"web chat runtime skill\"\n";
        out << "+++\n\n";
        out << "Skill body for web runtime tests.\n";
    }
    {
        const auto hook = hook_root / "before_tool_call" / "01-web-chat-hook.sh";
        std::ofstream out(hook);
        out << "#!/bin/sh\n";
        out << "exit 0\n";
        out.close();
        std::filesystem::permissions(hook, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    auto config = make_config();
    config.skill_paths = {skill_root.string()};
    config.hook_paths = {hook_root.string()};
    config.agents["default"].workspace = workspace.string();

    MemoryStore memory_store((workspace / "memory.db").string());
    SubagentRunStore run_store((workspace / "runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    std::string session_id = "web-chat-skills-hooks";

    auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, &subagent_manager, nullptr);

    EXPECT_NE(runtime.skills_prompt.find("web-chat-runtime-skill"), std::string::npos);
    ASSERT_NE(runtime.hook_manager, nullptr);
    EXPECT_EQ(runtime.hook_manager->hook_count(HookEvent::before_tool_call), 1);
}

TEST(WebChatTest, CompletionResumeAfterSessionShutdownFallsBackToInboxNotes) {
    const auto automation_db_path = std::filesystem::temp_directory_path() / ("orangutan-web-chat-automation-" + std::to_string(getpid()) + ".db");
    std::filesystem::remove(automation_db_path);
    auto automation_store = std::make_shared<automation::Store>(automation_db_path.string());
    automation::Runtime automation_runtime(*automation_store);
    ToolRegistry tools;
    size_t provider_calls = 0;
    ScriptedProvider provider({
        [&provider_calls](const std::vector<Message> &) {
            ++provider_calls;
            LLMResponse response;
            response.stop_reason = "end_turn";
            response.content.emplace_back(TextBlock{.text = "should not run"});
            return response;
        },
    });
    AgentLoop agent(provider, tools, "You are a test agent.");

    auto resume_state = std::make_shared<WebCompletionResumeState>();
    resume_state->agent = &agent;
    resume_state->agent_key = "default";
    resume_state->automation_runtime = &automation_runtime;
    {
        std::scoped_lock lock(resume_state->mutex);
        resume_state->agent = nullptr;
    }

    ToolRuntimeContext tool_context{
        .runtime_key = "agent:default|web:local",
        .agent_key = "default",
        .scope_key = "agent:default|web",
        .automation_runtime = &automation_runtime,
        .background_completion_runtime = make_background_completion_runtime_bindings(
            [automation_store](const automation::InboxItem &item) {
                static_cast<void>(automation_store->insert_inbox(item));
            },
            web::detail::make_web_completion_resume_callback(resume_state)),
    };
    BackgroundCompletionDispatcher dispatcher(&tool_context);

    dispatcher.dispatch(BackgroundProcessCompletionEvent{
        .process_id = "proc-web",
        .command = "sleep 1",
        .working_dir = orangutan::testing::test_tmp_root().string(),
        .pid = 1234,
        .terminal_status = BackgroundProcessTerminalStatus::exited,
        .exit_code = 0,
        .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
        .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
    });

    EXPECT_EQ(provider_calls, 0U);
    const auto inbox_items = automation_runtime.list_inbox("default");
    ASSERT_EQ(inbox_items.size(), 2U);

    const auto *completion_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion");
    const auto *failure_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion_resume_failure");
    ASSERT_NE(completion_item, nullptr);
    ASSERT_NE(failure_item, nullptr);
    EXPECT_EQ(json::parse(failure_item->body).at("reason"), "web session is no longer live");
    std::filesystem::remove(automation_db_path);
}

TEST(WebChatTest, PendingApprovalDenialReturnsFalse) {
    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions;
    auto session = std::make_unique<WebSessionState>();
    session->session_id = "web-session";
    auto *session_ptr = session.get();
    sessions.emplace(session->session_id, std::move(session));

    std::optional<nlohmann::json> event_payload;
    std::mutex event_mutex;
    std::condition_variable event_cv;
    std::promise<bool> approval_result;
    auto approval_future = approval_result.get_future();

    std::thread waiter([&] {
        approval_result.set_value(web::detail::await_web_approval(
            *session_ptr, sessions_mutex, ToolUseBlock{.id = "shell-deny", .name = "shell", .input = {{"command", "echo hello"}}}, ToolSandboxMode::isolated,
            "Shell command approval required.",
            [&](std::string_view, const json &payload) {
                std::lock_guard lock(event_mutex);
                event_payload = payload;
                event_cv.notify_one();
                return true;
            },
            {}, std::chrono::seconds(5)));
    });

    {
        std::unique_lock lock(event_mutex);
        ASSERT_TRUE(event_cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return event_payload.has_value();
        }));
    }

    httplib::Request req;
    req.body =
        nlohmann::json{
            {"session_id", "web-session"},
            {"request_id", (*event_payload)["request_id"]},
            {"approved", false},
        }
            .dump();
    httplib::Response res;
    web::handle_chat_approval(req, res, sessions_mutex, sessions);

    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(nlohmann::json::parse(res.body)["status"], "denied");
    EXPECT_FALSE(approval_future.get());
    waiter.join();
}

TEST(WebChatTest, AbortEndpointCancelsPendingApproval) {
    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions;
    auto session = std::make_unique<WebSessionState>();
    session->session_id = "web-session";
    auto *session_ptr = session.get();
    sessions.emplace(session->session_id, std::move(session));

    std::promise<bool> approval_result;
    auto approval_future = approval_result.get_future();
    std::thread waiter([&] {
        approval_result.set_value(web::detail::await_web_approval(
            *session_ptr, sessions_mutex, ToolUseBlock{.id = "shell-abort", .name = "shell", .input = {{"command", "echo hello"}}}, ToolSandboxMode::isolated,
            "Shell command approval required.",
            [](std::string_view, const json &) {
                return true;
            },
            {}, std::chrono::seconds(5)));
    });

    for (int attempt = 0; attempt < 50; ++attempt) {
        {
            std::lock_guard lock(sessions_mutex);
            if (session_ptr->pending_approval != nullptr) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::lock_guard lock(sessions_mutex);
        ASSERT_NE(session_ptr->pending_approval, nullptr);
    }

    httplib::Request req;
    req.body = R"({"session_id":"web-session"})";
    httplib::Response res;
    web::handle_chat_abort(req, res, sessions_mutex, sessions);

    EXPECT_EQ(nlohmann::json::parse(res.body)["status"], "abort_requested");
    EXPECT_TRUE(session_ptr->abort_requested.load());
    EXPECT_FALSE(approval_future.get());
    waiter.join();
}

TEST(WebChatTest, PendingApprovalTimeoutBehavesAsDenied) {
    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions;
    auto session = std::make_unique<WebSessionState>();
    session->session_id = "web-session";
    auto *session_ptr = session.get();
    sessions.emplace(session->session_id, std::move(session));

    auto approved = web::detail::await_web_approval(
        *session_ptr, sessions_mutex, ToolUseBlock{.id = "shell-timeout", .name = "shell", .input = {{"command", "echo hello"}}}, ToolSandboxMode::isolated,
        "Shell command approval required.",
        [](std::string_view, const json &) {
            return true;
        },
        {}, std::chrono::milliseconds(50));

    EXPECT_FALSE(approved);
    std::lock_guard lock(sessions_mutex);
    EXPECT_EQ(session_ptr->pending_approval, nullptr);
}

TEST(WebChatTest, PendingApprovalCleanupCancelsUnresolvedRequest) {
    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions;
    auto session = std::make_unique<WebSessionState>();
    session->session_id = "web-session";
    auto *session_ptr = session.get();
    sessions.emplace(session->session_id, std::move(session));

    std::promise<bool> approval_result;
    auto approval_future = approval_result.get_future();
    std::thread waiter([&] {
        approval_result.set_value(web::detail::await_web_approval(
            *session_ptr, sessions_mutex, ToolUseBlock{.id = "shell-cleanup", .name = "shell", .input = {{"command", "echo hello"}}}, ToolSandboxMode::isolated,
            "Shell command approval required.",
            [](std::string_view, const json &) {
                return true;
            },
            {}, std::chrono::seconds(5)));
    });

    for (int attempt = 0; attempt < 50; ++attempt) {
        {
            std::lock_guard lock(sessions_mutex);
            if (session_ptr->pending_approval != nullptr) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::lock_guard lock(sessions_mutex);
        ASSERT_NE(session_ptr->pending_approval, nullptr);
    }

    web::detail::cancel_pending_approval(*session_ptr);

    EXPECT_FALSE(approval_future.get());
    waiter.join();
}

} // namespace orangutan
