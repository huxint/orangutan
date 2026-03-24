#include "features/web/web-server.hpp"
#include "features/web/web-routes.hpp"
#include "app/runtime/app-runtime.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "infra/config/config.hpp"
#include "test-helpers.hpp"
#include "support/ut.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using orangutan::testing::ScopedEnvVar;

namespace {

class WebRoutesHarness {
public:
    WebRoutesHarness()
    : temp_root_(orangutan::testing::unique_test_root("web-routes")),
      home_root_(temp_root_ / "home"),
      db_path_((temp_root_ / "sessions.db").string()),
      home_env_("HOME", home_root_.string()),
      session_store_(std::make_unique<orangutan::SessionStore>(db_path_)) {
        std::filesystem::create_directories(home_root_ / ".orangutan");
    }

    [[nodiscard]]
    std::filesystem::path config_path() const {
        return home_root_ / ".orangutan" / "config.toml";
    }

    [[nodiscard]]
    orangutan::SessionStore *session_store() const {
        return session_store_.get();
    }

    [[nodiscard]]
    const std::filesystem::path &temp_root() const {
        return temp_root_;
    }

private:
    std::filesystem::path temp_root_;
    std::filesystem::path home_root_;
    std::string db_path_;
    ScopedEnvVar home_env_;
    std::unique_ptr<orangutan::SessionStore> session_store_;
};

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

boost::ut::suite web_routes_suite = [] {
    using namespace boost::ut;

    "list_sessions_returns_empty_array"_test = [] {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/sessions");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body.is_array());
        expect(body.empty());
        server.stop();
    };

    "get_session_returns_404_for_missing"_test = [] {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/sessions/nonexistent");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 404_i);
        server.stop();
    };

    "delete_session_returns_ok"_test = [] {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Delete("/api/sessions/nonexistent");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        server.stop();
    };

    "get_config_returns_json"_test = [] {
        orangutan::Config cfg;
        cfg.model = "test-model";
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/config");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body["model"] == "test-model");
        expect(not body.contains("api_key"));
        server.stop();
    };

    "get_config_returns_503_when_null"_test = [] {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/config");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 503_i);
        server.stop();
    };

    "put_config_updates_model"_test = [] {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.model = "old-model";
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_config_save_path(harness.config_path());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Put("/api/config", R"({"model":"new-model"})", "application/json");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        expect(cfg.model == "new-model");
        expect(std::filesystem::exists(harness.config_path()));
        server.stop();
    };

    "list_tools_returns_503_when_null"_test = [] {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/tools");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 503_i);
        server.stop();
    };

    "list_agents_returns_array"_test = [] {
        orangutan::Config cfg;
        cfg.model = "default-model";
        cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model", .subagents = {"helper"}};
        cfg.agents["helper"] = orangutan::AgentConfig{.model = "helper-model"};
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/agents");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body.is_array());
        expect(body.size() == 2_ul);
        bool saw_default = false;
        bool saw_helper = false;
        for (const auto &agent : body) {
            if (agent["key"] == "default") {
                saw_default = true;
                expect(agent["subagents"][0] == "helper");
            }
            if (agent["key"] == "helper") {
                saw_helper = true;
            }
        }
        expect(saw_default);
        expect(saw_helper);
        server.stop();
    };

    "list_agent_sessions_returns_only_matching_agent_sessions"_test = [] {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model"};
        cfg.agents["coder"] = orangutan::AgentConfig{.model = "coder-model"};

        harness.session_store()->save({orangutan::Message::user_text("default")}, make_session_metadata("default-model", "agent:default|web", "default", "web", "web:local"));
        harness.session_store()->save({orangutan::Message::user_text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));
        harness.session_store()->save({orangutan::Message::user_text("channel")},
                                      make_session_metadata("coder-model", "agent:coder|jid:qqbot:c2c:42", "coder", "channel", "qqbot:c2c:42"));

        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/agents/coder/sessions");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body.is_array() >> fatal);
        expect(body.size() == 2_ul);
        expect(body[0]["agent_key"] == "coder");
        expect(body[1]["agent_key"] == "coder");
        expect(body[0]["read_only"].get<bool>() || body[1]["read_only"].get<bool>());
        server.stop();
    };

    "get_agent_session_rejects_cross_agent_access"_test = [] {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model"};
        cfg.agents["coder"] = orangutan::AgentConfig{.model = "coder-model"};
        const auto coder_session_id =
            harness.session_store()->save({orangutan::Message::user_text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));

        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/agents/default/sessions/" + coder_session_id);
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 404_i);
        server.stop();
    };

    "system_status_returns_uptime"_test = [] {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/system/status");
        expect(static_cast<bool>(res) >> fatal);
        expect(res->status == 200_i);
        const auto body = nlohmann::json::parse(res->body);
        expect(body.contains("uptime_seconds"));
        expect(body.contains("active_web_sessions"));
        server.stop();
    };

    "shared_web_runtime_builds_with_parity_context_and_tools"_test = [] {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "runtime-workspace";
        std::filesystem::create_directories(workspace);

        auto cfg = make_runtime_config(workspace);
        orangutan::app::AppRuntime app_runtime((harness.temp_root() / "automation.db"));
        orangutan::MemoryStore memory_store((harness.temp_root() / "memory.db"));
        orangutan::SubagentRunStore run_store((harness.temp_root() / "subagent-runs.db"));
        orangutan::SubagentManager subagent_manager(run_store, [](const orangutan::SubagentWorkerRequest &) {
            return orangutan::SubagentWorkerResult{.status = orangutan::SubagentRunStatus::succeeded};
        });
        std::string session_id = "web-session";

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &subagent_manager, &app_runtime.automation_runtime(),
                                                                        [](const orangutan::ToolUseBlock &, const std::string &) {
                                                                            return false;
                                                                        });

        const auto definitions = runtime.tools.definitions();
        expect(has_tool_named(definitions, "memory_list"));
        expect(has_tool_named(definitions, "shell"));
        expect(has_tool_named(definitions, "custom_echo"));
        expect(has_tool_named(definitions, "task"));
        expect(has_tool_named(definitions, "heartbeat"));
        expect(has_tool_named(definitions, "inbox"));
        expect(runtime.tool_context.runtime_origin == orangutan::SubagentRuntimeOrigin::web);
        expect(runtime.tool_context.raw_caller_id == "web:local");
        expect(runtime.tool_context.current_session_id == &session_id);
        expect(runtime.tool_context.allowed_child_agents == std::vector<std::string>({"coder"}));
        expect(runtime.tool_context.automation_runtime == &app_runtime.automation_runtime());
        expect(static_cast<bool>(runtime.tool_context.approval_callback));
        expect(runtime.agent != nullptr);

        const auto shell = std::ranges::find_if(definitions, [](const orangutan::ToolDef &definition) {
            return definition.name == "shell";
        });
        expect((shell != definitions.end()) >> fatal);
        expect(shell->input_schema.contains("properties"));
        expect(not shell->input_schema["properties"].contains("on_complete"));

        const auto shell_result = runtime.tools.execute(orangutan::ToolUseBlock{
            .id = "web-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(shell_result.is_error);
        expect(shell_result.content.contains("requires approval") || shell_result.content.contains("rejected by user"));
    };

    "shared_web_runtime_loads_skills_and_hooks"_test = [] {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "skills-hooks-workspace";
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
        orangutan::MemoryStore memory_store((harness.temp_root() / "memory-skills.db"));
        orangutan::SubagentRunStore run_store((harness.temp_root() / "subagent-runs-skills.db"));
        orangutan::SubagentManager subagent_manager(run_store, [](const orangutan::SubagentWorkerRequest &) {
            return orangutan::SubagentWorkerResult{.status = orangutan::SubagentRunStatus::succeeded};
        });
        std::string session_id = "web-session-skills";

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &subagent_manager, nullptr);

        expect(runtime.skills_prompt.contains("web-runtime-skill"));
        expect((runtime.hook_manager != nullptr) >> fatal);
        expect(runtime.hook_manager->hook_count(orangutan::HookEvent::before_tool_call) == 1_i);
    };

    "automation_endpoints_expose_shared_state"_test = [] {
        WebRoutesHarness harness;
        orangutan::app::AppRuntime app_runtime((harness.temp_root() / "automation.db"));
        auto &automation_runtime = app_runtime.automation_runtime();
        orangutan::automation::TaskSpec task_spec;
        task_spec.agent_key = "default";
        task_spec.name = "repo-check";
        task_spec.schedule.kind = orangutan::automation::TaskScheduleKind::cron;
        task_spec.schedule.value = "0 * * * *";
        task_spec.prompt = "Check the repository state.";
        const auto task_id = automation_runtime.save_task(task_spec);
        const auto heartbeat_id = automation_runtime.save_heartbeat(orangutan::automation::HeartbeatSpec{
            .agent_key = "default",
            .name = "self-check",
            .every_seconds = 1800,
            .jitter_seconds = 300,
            .prompt = "Wake up and inspect ongoing work.",
        });
        const auto inserted_inbox_id = automation_runtime.store().insert_inbox(orangutan::automation::InboxItem{
            .agent_key = "default",
            .source_kind = "task",
            .source_run_id = "run-1",
            .title = "Task notification",
            .body = "Repository check found changes.",
            .created_at = 123,
        });
        expect(not task_id.empty());
        expect(not heartbeat_id.empty());
        expect(not inserted_inbox_id.empty());

        orangutan::WebServer server;
        server.set_automation_runtime(&automation_runtime);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto tasks_res = cli.Get("/api/tasks?agent_key=default");
        expect(static_cast<bool>(tasks_res) >> fatal);
        expect(tasks_res->status == 200_i);
        const auto tasks = nlohmann::json::parse(tasks_res->body);
        expect(tasks.size() == 1_ul);
        expect(tasks[0]["name"] == "repo-check");
        expect(tasks[0]["schedule_kind"] == "cron");

        const auto heartbeats_res = cli.Get("/api/heartbeats?agent_key=default");
        expect(static_cast<bool>(heartbeats_res) >> fatal);
        expect(heartbeats_res->status == 200_i);
        const auto heartbeats = nlohmann::json::parse(heartbeats_res->body);
        expect(heartbeats.size() == 1_ul);
        expect(heartbeats[0]["name"] == "self-check");

        const auto inbox_res = cli.Get("/api/inbox?agent_key=default");
        expect(static_cast<bool>(inbox_res) >> fatal);
        expect(inbox_res->status == 200_i);
        const auto inbox = nlohmann::json::parse(inbox_res->body);
        expect(inbox.size() == 1_ul);
        const auto inbox_id = inbox[0]["id"].get<std::string>();
        expect(inbox[0]["title"] == "Task notification");

        const auto ack_res = cli.Post("/api/inbox/ack", nlohmann::json{{"agent_key", "default"}, {"id", inbox_id}}.dump(), "application/json");
        expect(static_cast<bool>(ack_res) >> fatal);
        expect(ack_res->status == 200_i);

        const auto inbox_after_ack = cli.Get("/api/inbox?agent_key=default");
        expect(static_cast<bool>(inbox_after_ack) >> fatal);
        expect(inbox_after_ack->status == 200_i);
        expect(nlohmann::json::parse(inbox_after_ack->body)[0]["status"] == "acked");

        const auto clear_res = cli.Delete("/api/inbox?agent_key=default");
        expect(static_cast<bool>(clear_res) >> fatal);
        expect(clear_res->status == 200_i);

        const auto inbox_after_clear = cli.Get("/api/inbox?agent_key=default");
        expect(static_cast<bool>(inbox_after_clear) >> fatal);
        expect(inbox_after_clear->status == 200_i);
        expect(nlohmann::json::parse(inbox_after_clear->body).empty());
        server.stop();
    };

    "chat_approval_endpoint_rejects_invalid_approval_payload"_test = [] {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::WebSessionState>> sessions;

        httplib::Request req;
        req.body = R"({"session_id":"web-session","approved":true})";
        httplib::Response res;

        orangutan::web::handle_chat_approval(req, res, sessions_mutex, sessions);

        expect(res.status == 400_i);
        expect(nlohmann::json::parse(res.body)["error"] == "missing or invalid 'request_id' field");
    };

    "chat_approval_endpoint_approves_pending_request"_test = [] {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::WebSessionState>> sessions;
        auto session = std::make_unique<orangutan::WebSessionState>();
        session->session_id = "web-session";
        auto *session_ptr = session.get();
        sessions.emplace(session->session_id, std::move(session));

        std::mutex event_mutex;
        std::condition_variable event_cv;
        std::optional<std::string> event_name;
        std::optional<nlohmann::json> event_payload;
        std::promise<bool> approval_result;
        auto approval_future = approval_result.get_future();

        std::thread waiter([&] {
            approval_result.set_value(orangutan::web::detail::await_web_approval(
                *session_ptr, sessions_mutex, orangutan::ToolUseBlock{.id = "shell-approval", .name = "shell", .input = {{"command", "echo hello"}}},
                orangutan::ToolSandboxMode::isolated, "Shell command approval required.",
                [&](std::string_view current_event_name, const orangutan::json &payload) {
                    std::scoped_lock lock(event_mutex);
                    event_name = std::string(current_event_name);
                    event_payload = payload;
                    event_cv.notify_one();
                    return true;
                },
                {}, std::chrono::seconds(5)));
        });

        {
            std::unique_lock lock(event_mutex);
            expect(event_cv.wait_for(lock, std::chrono::seconds(1), [&] {
                return event_payload.has_value();
            }) >> fatal);
        }

        expect(event_name.has_value() >> fatal);
        expect(event_payload.has_value() >> fatal);
        expect(*event_name == "approval_request");
        expect((*event_payload)["tool"] == "shell");
        expect((*event_payload)["sandbox_mode"] == "isolated");
        expect((*event_payload)["command"] == "echo hello");

        httplib::Request req;
        req.body =
            nlohmann::json{
                {"session_id", "web-session"},
                {"request_id", (*event_payload)["request_id"]},
                {"approved", true},
            }
                .dump();
        httplib::Response res;

        orangutan::web::handle_chat_approval(req, res, sessions_mutex, sessions);

        expect(res.status == 200_i);
        expect(nlohmann::json::parse(res.body)["status"] == "approved");
        expect(approval_future.get());
        waiter.join();
    };

    "chat_approval_endpoint_rejects_mismatched_request_id"_test = [] {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::WebSessionState>> sessions;
        auto session = std::make_unique<orangutan::WebSessionState>();
        session->session_id = "web-session";
        auto *session_ptr = session.get();
        sessions.emplace(session->session_id, std::move(session));

        std::promise<bool> approval_result;
        auto approval_future = approval_result.get_future();
        std::thread waiter([&] {
            approval_result.set_value(orangutan::web::detail::await_web_approval(
                *session_ptr, sessions_mutex, orangutan::ToolUseBlock{.id = "shell-approval", .name = "shell", .input = {{"command", "echo hello"}}},
                orangutan::ToolSandboxMode::isolated, "Shell command approval required.",
                [](std::string_view, const orangutan::json &) {
                    return true;
                },
                {}, std::chrono::seconds(5)));
        });

        for (int attempt = 0; attempt < 50; ++attempt) {
            {
                std::scoped_lock lock(sessions_mutex);
                if (session_ptr->pending_approval != nullptr) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::scoped_lock lock(sessions_mutex);
            expect((session_ptr->pending_approval != nullptr) >> fatal);
        }

        httplib::Request req;
        req.body =
            nlohmann::json{
                {"session_id", "web-session"},
                {"request_id", "approval-mismatch"},
                {"approved", true},
            }
                .dump();
        httplib::Response res;

        orangutan::web::handle_chat_approval(req, res, sessions_mutex, sessions);

        expect(res.status == 404_i);
        expect(nlohmann::json::parse(res.body)["error"] == "approval not found");

        orangutan::web::detail::cancel_pending_approval(*session_ptr);
        expect(not approval_future.get());
        waiter.join();
    };
};

} // namespace
