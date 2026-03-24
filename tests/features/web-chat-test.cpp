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
#include "support/ut.hpp"
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

    LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(std::string_view, const std::vector<Message> &messages, const std::vector<ToolDef> &, const StreamCallback &, int) override {
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

class WebChatStoreHarness {
public:
    WebChatStoreHarness()
    : db_path_(orangutan::testing::unique_test_db_path("web-chat", "sessions.db")),
      store_(db_path_.string()) {}

    ~WebChatStoreHarness() {
        std::filesystem::remove_all(db_path_.parent_path());
    }

    SessionStore &store() {
        return store_;
    }

private:
    std::filesystem::path db_path_;
    SessionStore store_;
};

class WebChatServerHarness {
public:
    WebChatServerHarness(Config *config = nullptr, SessionStore *store = nullptr, automation::Runtime *automation_runtime = nullptr) {
        if (config != nullptr) {
            server_.set_config(config);
        }
        if (store != nullptr) {
            server_.set_session_store(store);
        }
        if (automation_runtime != nullptr) {
            server_.set_automation_runtime(automation_runtime);
        }
        server_.start("127.0.0.1", 0);
        client_ = std::make_unique<httplib::Client>("127.0.0.1", server_.port());
    }

    ~WebChatServerHarness() {
        server_.stop();
    }

    httplib::Client &client() {
        return *client_;
    }

private:
    WebServer server_;
    std::unique_ptr<httplib::Client> client_;
};

boost::ut::suite web_chat_suite = [] {
    using namespace boost::ut;

    "chat_endpoint_rejects_missing_message"_test = [] {
        Config config = make_config();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", "{}", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 400_i);
    };

    "chat_endpoint_rejects_invalid_json"_test = [] {
        Config config = make_config();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", "not json", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 400_i);
    };

    "chat_endpoint_returns_503_without_config"_test = [] {
        WebChatServerHarness harness;

        const auto res = harness.client().Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 503_i);
    };

    "chat_endpoint_rejects_missing_agent_key"_test = [] {
        Config config = make_config();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", R"({"message":"hello"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 400_i);
    };

    "chat_endpoint_rejects_missing_api_key"_test = [] {
        orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
        orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

        Config config = make_config();
        config.api_key.clear();
        config.agents["default"].api_key.clear();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 400_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body["error"] == "missing API key for agent 'default'");
    };

    "help_slash_command_streams_help_without_api_key"_test = [] {
        orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
        orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

        Config config = make_config();
        config.api_key.clear();
        config.agents["default"].api_key.clear();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", R"({"message":"/help","agent_key":"default"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto text_event = find_sse_event_payload(res->body, "text");
        expect(text_event.has_value() >> fatal);
        expect(text_event->at("text").get<std::string>().find("/tasks run <id>") != std::string::npos);
    };

    "chat_endpoint_rejects_invalid_workspace_config"_test = [] {
        const auto workspace_file = orangutan::testing::test_tmp_root() / "web-chat-invalid-workspace";
        std::filesystem::remove(workspace_file);
        {
            std::ofstream out(workspace_file);
            out << "not a directory\n";
        }

        Config config = make_config();
        config.agents["default"].workspace = workspace_file.string();
        WebChatServerHarness harness(&config);

        const auto res = harness.client().Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 500_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body["error"].get<std::string>().find("failed to resolve workspace for agent 'default'") != std::string::npos);

        std::filesystem::remove(workspace_file);
    };

    "chat_endpoint_rejects_read_only_channel_session"_test = [] {
        WebChatStoreHarness store_harness;
        Config config = make_config();
        const auto session_id =
            store_harness.store().save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
        WebChatServerHarness harness(&config, &store_harness.store());

        const auto res = harness.client().Post("/api/chat", json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 409_i);
    };

    "chat_endpoint_rejects_cross_agent_session_access"_test = [] {
        WebChatStoreHarness store_harness;
        Config config = make_config();
        const auto session_id = store_harness.store().save({Message::user_text("hello")}, make_session_metadata("coder-test", "agent:coder|web", "coder", "web", "web:local"));
        WebChatServerHarness harness(&config, &store_harness.store());

        const auto res = harness.client().Post("/api/chat", json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 404_i);
    };

    "resume_slash_command_streams_target_session_event"_test = [] {
        WebChatStoreHarness store_harness;
        Config config = make_config();
        config.api_key.clear();
        config.agents["default"].api_key.clear();
        const auto session_id = store_harness.store().save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));
        WebChatServerHarness harness(&config, &store_harness.store());

        const auto res = harness.client().Post("/api/chat", json{{"message", std::string("/resume ") + session_id}, {"agent_key", "default"}}.dump(), "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto session_event = find_sse_event_payload(res->body, "session");
        expect(session_event.has_value() >> fatal);
        expect(session_event->at("session_id") == session_id);
        const auto text_event = find_sse_event_payload(res->body, "text");
        expect(text_event.has_value() >> fatal);
        expect(text_event->at("text").get<std::string>().find(session_id) != std::string::npos);
    };

    "new_slash_command_streams_markdown_reply_and_session_event"_test = [] {
        WebChatStoreHarness store_harness;
        Config config = make_config();
        config.api_key.clear();
        config.agents["default"].api_key.clear();
        const auto existing_session_id =
            store_harness.store().save({Message::user_text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));
        WebChatServerHarness harness(&config, &store_harness.store());

        const auto res = harness.client().Post("/api/chat", json{{"message", "/new"}, {"agent_key", "default"}, {"session_id", existing_session_id}}.dump(), "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto session_event = find_sse_event_payload(res->body, "session");
        expect(session_event.has_value() >> fatal);
        expect(session_event->at("session_id") != existing_session_id);
        expect(not find_sse_event_payload(res->body, "text").has_value());
    };

    "export_slash_command_works_for_read_only_channel_session"_test = [] {
        WebChatStoreHarness store_harness;
        Config config = make_config();
        config.api_key.clear();
        config.agents["default"].api_key.clear();
        const auto workspace = orangutan::testing::unique_test_root("web-chat-export");
        config.agents["default"].workspace = workspace.string();

        const auto session_id = store_harness.store().save({Message::user_text("hello"), Message::assistant_text("copied reply")},
                                                           make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
        const auto export_path = workspace / ".exports" / (session_id + ".md");
        WebChatServerHarness harness(&config, &store_harness.store());

        const auto res = harness.client().Post("/api/chat", json{{"message", "/export"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto text_event = find_sse_event_payload(res->body, "text");
        expect(text_event.has_value() >> fatal);
        expect(text_event->at("text") == "## Export\n- Saved current session to `" + export_path.string() + '`');
        expect(std::filesystem::exists(export_path) >> fatal);

        std::ifstream in(export_path);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(content.find("# Session Export") != std::string::npos);
        expect(content.find("hello") != std::string::npos);
        expect(content.find("copied reply") != std::string::npos);

        std::filesystem::remove_all(workspace);
    };

    "abort_endpoint_returns_404_for_unknown_session"_test = [] {
        WebChatServerHarness harness;

        const auto res = harness.client().Post("/api/chat/abort", R"({"session_id":"nonexistent"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 404_i);
    };

    "abort_endpoint_rejects_missing_session_id"_test = [] {
        WebChatServerHarness harness;

        const auto res = harness.client().Post("/api/chat/abort", "{}", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 400_i);
    };

    "runtime_bundle_includes_custom_and_memory_tools"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("web-chat-runtime-workspace");

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

        MemoryStore memory_store((workspace / "memory.db"));
        SubagentRunStore run_store((workspace / "runs.db"));
        SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        std::string session_id = "web-chat-runtime-session";

        auto runtime =
            web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, &subagent_manager, nullptr, [](const ToolUseBlock &, const std::string &) {
                return false;
            });

        expect(orangutan::testing::has_tool_named(runtime.tools.definitions(), "memory_list"));
        expect(orangutan::testing::has_tool_named(runtime.tools.definitions(), "custom_echo"));
        expect(runtime.tool_context.runtime_origin == SubagentRuntimeOrigin::web);
        expect(runtime.tool_context.raw_caller_id == "web:local");
        expect(runtime.tool_context.current_session_id == &session_id);
        expect(runtime.tool_context.allowed_child_agents == std::vector<std::string>{"coder"});
        expect(static_cast<bool>(runtime.tool_context.approval_callback));
        expect((runtime.agent != nullptr) >> fatal);

        const auto shell_result = runtime.tools.execute(ToolUseBlock{
            .id = "web-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(shell_result.is_error);
        expect(shell_result.content.find("requires approval") != std::string::npos || shell_result.content.find("rejected by user") != std::string::npos);
    };

    "tasks_slash_command_uses_runtime_tool_output"_test = [] {
        Config config = make_config();
        auto automation_store = std::make_shared<automation::Store>(orangutan::testing::unique_test_db_path("web-chat-tasks", "automation.db"));
        automation::Runtime automation_runtime(*automation_store);
        WebChatServerHarness harness(&config, nullptr, &automation_runtime);

        const auto res = harness.client().Post("/api/chat", R"({"message":"/tasks","agent_key":"default"})", "application/json");

        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto text_event = find_sse_event_payload(res->body, "text");
        expect(text_event.has_value() >> fatal);
        expect(text_event->at("text") == "## Tasks\n- 🗓️ No tasks configured.");
    };

    "runtime_bundle_loads_skills_and_hooks_from_configured_paths"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("web-chat-runtime-skills-hooks");
        const auto skill_root = workspace / "skills";
        const auto hook_root = workspace / "hooks";
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

        MemoryStore memory_store((workspace / "memory.db"));
        SubagentRunStore run_store((workspace / "runs.db"));
        SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        std::string session_id = "web-chat-skills-hooks";

        auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, &subagent_manager, nullptr);

        expect(runtime.skills_prompt.find("web-chat-runtime-skill") != std::string::npos);
        expect((runtime.hook_manager != nullptr) >> fatal);
        expect(runtime.hook_manager->hook_count(HookEvent::before_tool_call) == 1_i);
    };

    "completion_resume_after_session_shutdown_falls_back_to_inbox_notes"_test = [] {
        const auto automation_db_path = orangutan::testing::unique_test_db_path("web-chat-automation", "automation.db");
        auto automation_store = std::make_shared<automation::Store>(automation_db_path);
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
            .working_dir = orangutan::testing::unique_test_root("web-chat-completion").string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
        });

        expect(provider_calls == 0_ul);
        const auto inbox_items = automation_runtime.list_inbox("default");
        expect((inbox_items.size() == 2_ul) >> fatal);

        const auto *completion_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion");
        const auto *failure_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion_resume_failure");
        expect((completion_item != nullptr) >> fatal);
        expect((failure_item != nullptr) >> fatal);
        expect(json::parse(failure_item->body).at("reason") == "web session is no longer live");
        std::filesystem::remove(automation_db_path);
    };

    "pending_approval_denial_returns_false"_test = [] {
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
            expect((event_cv.wait_for(lock, std::chrono::seconds(1),
                                      [&] {
                                          return event_payload.has_value();
                                      })) >>
                   fatal);
        }
        expect(event_payload.has_value() >> fatal);

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

        expect(res.status == 200_i);
        expect(nlohmann::json::parse(res.body)["status"] == "denied");
        expect(not approval_future.get());
        waiter.join();
    };

    "abort_endpoint_cancels_pending_approval"_test = [] {
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
            expect((session_ptr->pending_approval != nullptr) >> fatal);
        }

        httplib::Request req;
        req.body = R"({"session_id":"web-session"})";
        httplib::Response res;
        web::handle_chat_abort(req, res, sessions_mutex, sessions);

        expect(nlohmann::json::parse(res.body)["status"] == "abort_requested");
        expect(session_ptr->abort_requested.load());
        expect(not approval_future.get());
        waiter.join();
    };

    "pending_approval_timeout_behaves_as_denied"_test = [] {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions;
        auto session = std::make_unique<WebSessionState>();
        session->session_id = "web-session";
        auto *session_ptr = session.get();
        sessions.emplace(session->session_id, std::move(session));

        const auto approved = web::detail::await_web_approval(
            *session_ptr, sessions_mutex, ToolUseBlock{.id = "shell-timeout", .name = "shell", .input = {{"command", "echo hello"}}}, ToolSandboxMode::isolated,
            "Shell command approval required.",
            [](std::string_view, const json &) {
                return true;
            },
            {}, std::chrono::milliseconds(50));

        expect(not approved);
        std::lock_guard lock(sessions_mutex);
        expect(session_ptr->pending_approval == nullptr);
    };

    "pending_approval_cleanup_cancels_unresolved_request"_test = [] {
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
            expect((session_ptr->pending_approval != nullptr) >> fatal);
        }

        web::detail::cancel_pending_approval(*session_ptr);

        expect(not approval_future.get());
        waiter.join();
    };
};

} // namespace

} // namespace orangutan
