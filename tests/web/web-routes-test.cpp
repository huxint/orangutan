#include "web/web-server.hpp"
#include "web/web-routes.hpp"
#include "bootstrap/app-runtime.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "storage/session-store.hpp"
#include "config/config.hpp"
#include "test-helpers.hpp"
#include <catch2/catch_test_macros.hpp>
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

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

namespace {

    ProfileConfig make_profile(std::initializer_list<std::pair<const std::string, ModelConfig>> models, std::string api_key = "test-key",
                               std::string base_url = "https://example.test") {
        ProfileConfig profile{
            .base_url = std::move(base_url),
            .api_key = std::move(api_key),
        };
        for (const auto &[name, model] : models) {
            profile.models.emplace(name, model);
        }
        return profile;
    }

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
            return home_root_ / ".orangutan" / "config.json";
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

    orangutan::Config make_runtime_config(const std::filesystem::path &workspace_root) {
        orangutan::Config cfg;
        cfg.profile = "shared";
        cfg.model = "gpt-test";
        cfg.profiles.emplace("shared", make_profile({{"gpt-test", ModelConfig{.endpoint_style = "openai-chat-completions"}},
                                                     {"gpt-coder-test", ModelConfig{.endpoint_style = "openai-chat-completions"}}}));
        cfg.custom_tools.push_back(orangutan::Config::ScriptToolConfig{
            .name = "custom_echo",
            .description = "Custom echo tool",
            .command = "echo hello",
        });
        cfg.agents["default"] = orangutan::AgentConfig{
            .profile = "shared",
            .model = "gpt-test",
            .workspace = workspace_root.string(),
            .permissions_config =
                {
                    .default_mode = orangutan::PermissionMode::default_mode,
                },
            .team_agents = {"coder"},
        };
        cfg.agents["coder"] = orangutan::AgentConfig{
            .profile = "shared",
            .model = "gpt-coder-test",
            .workspace = workspace_root.string(),
        };
        return cfg;
    }

    TEST_CASE("list_sessions_returns_empty_array") {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/sessions");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body.is_array());
        CHECK(body.empty());
        server.stop();
    };

    TEST_CASE("get_session_returns_404_for_missing") {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/sessions/nonexistent");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 404);
        server.stop();
    };

    TEST_CASE("delete_session_returns_ok") {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Delete("/api/sessions/nonexistent");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        server.stop();
    };

    TEST_CASE("get_config_returns_json") {
        orangutan::Config cfg;
        cfg.model = "test-model";
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/config");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["agent"]["model"] == "test-model");
        CHECK_FALSE(body.contains("api_key"));
        server.stop();
    };

    TEST_CASE("get_config_returns_503_when_null") {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/config");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 503);
        server.stop();
    };

    TEST_CASE("put_config_updates_model") {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.model = "old-model";
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_config_save_path(harness.config_path());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Put("/api/config", R"({"agent":{"model":"new-model"}})", "application/json");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        CHECK(cfg.model == "new-model");
        CHECK(std::filesystem::exists(harness.config_path()));
        server.stop();
    };

    TEST_CASE("list_tools_returns_503_when_null") {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/tools");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 503);
        server.stop();
    };

    TEST_CASE("list_agents_returns_array") {
        orangutan::Config cfg;
        cfg.profile = "shared";
        cfg.model = "default-model";
        cfg.profiles.emplace("shared", make_profile({{"default-model", ModelConfig{.endpoint_style = "openai-chat-completions"}},
                                                     {"helper-model", ModelConfig{.endpoint_style = "openai-chat-completions"}}}));
        cfg.agents["default"] = orangutan::AgentConfig{.profile = "shared", .model = "default-model", .team_agents = {"helper"}};
        cfg.agents["helper"] = orangutan::AgentConfig{.profile = "shared", .model = "helper-model"};
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/agents");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body.is_array());
        CHECK(body.size() == 2UL);
        bool saw_default = false;
        bool saw_helper = false;
        for (const auto &agent : body) {
            if (agent["key"] == "default") {
                saw_default = true;
                CHECK(agent["team_agents"][0] == "helper");
            }
            if (agent["key"] == "helper") {
                saw_helper = true;
            }
        }
        CHECK(saw_default);
        CHECK(saw_helper);
        server.stop();
    };

    TEST_CASE("list_agent_sessions_returns_only_matching_agent_sessions") {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.profile = "shared";
        cfg.profiles.emplace("shared", make_profile({{"default-model", ModelConfig{.endpoint_style = "openai-chat-completions"}},
                                                     {"coder-model", ModelConfig{.endpoint_style = "openai-chat-completions"}}}));
        cfg.agents["default"] = orangutan::AgentConfig{.profile = "shared", .model = "default-model"};
        cfg.agents["coder"] = orangutan::AgentConfig{.profile = "shared", .model = "coder-model"};

        harness.session_store()->save({orangutan::Message::user().text("default")}, make_session_metadata("default-model", "agent:default|web", "default", "web", "web:local"));
        harness.session_store()->save({orangutan::Message::user().text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));
        harness.session_store()->save({orangutan::Message::user().text("channel")},
                                      make_session_metadata("coder-model", "agent:coder|jid:qqbot:c2c:42", "coder", "channel", "qqbot:c2c:42"));

        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/agents/coder/sessions");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.is_array());
        CHECK(body.size() == 2UL);
        CHECK(body[0]["agent_key"] == "coder");
        CHECK(body[1]["agent_key"] == "coder");
        CHECK((body[0]["read_only"].get<bool>() || body[1]["read_only"].get<bool>()));
        server.stop();
    };

    TEST_CASE("get_agent_session_rejects_cross_agent_access") {
        WebRoutesHarness harness;
        orangutan::Config cfg;
        cfg.agents["default"] = orangutan::AgentConfig{.model = "default-model"};
        cfg.agents["coder"] = orangutan::AgentConfig{.model = "coder-model"};
        const auto coder_session_id =
            harness.session_store()->save({orangutan::Message::user().text("coder")}, make_session_metadata("coder-model", "agent:coder|web", "coder", "web", "web:local"));

        orangutan::WebServer server;
        server.set_config(&cfg);
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/agents/default/sessions/" + coder_session_id);
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 404);
        server.stop();
    };

    TEST_CASE("system_status_returns_uptime") {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/system/status");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body.contains("uptime_seconds"));
        CHECK(body.contains("active_web_sessions"));
        server.stop();
    };

    TEST_CASE("shared_web_runtime_builds_with_parity_context_and_tools") {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "runtime-workspace";
        std::filesystem::create_directories(workspace);

        auto cfg = make_runtime_config(workspace);
        orangutan::bootstrap::AppRuntime app_runtime((harness.temp_root() / "automation.db"));
        orangutan::MemoryStore memory_store((harness.temp_root() / "memory.db"));
        std::string session_id = "web-session";

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &app_runtime.automation_runtime(),
                                                                        [](const orangutan::ToolUse &, const orangutan::PermissionDecision &) {
                                                                            return false;
                                                                        });

        const auto definitions = runtime.tools.definitions();
        CHECK(not(orangutan::testing::has_tool_named(definitions, "memory_list")));
        CHECK(orangutan::testing::has_tool_named(definitions, "shell"));
        CHECK(orangutan::testing::has_tool_named(definitions, "custom_echo"));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "task")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "heartbeat")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "inbox")));
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(runtime.tool_context.runtime_origin == base::origin::web);
        CHECK(runtime.tool_context.raw_caller_id == "web:local");
        CHECK(runtime.tool_context.current_session_id == &session_id);
        CHECK(runtime.tool_context.team_agents == std::vector<std::string>({"coder"}));
        CHECK(runtime.tool_context.automation_runtime == &app_runtime.automation_runtime());
        CHECK(runtime.tool_context.approval_callback != nullptr);
        CHECK(runtime.agent != nullptr);

        const auto shell = std::ranges::find_if(definitions, [](const orangutan::ToolDef &definition) {
            return definition.name == "shell";
        });
        REQUIRE(shell != definitions.end());
        CHECK(shell->input_schema.contains("properties"));
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));

        const auto shell_result = runtime.tools.execute(orangutan::ToolUse("web-shell", "shell", {{"command", "echo hello"}}));
        CHECK(shell_result.is_error);
        CHECK((shell_result.content.contains("requires approval") || shell_result.content.contains("rejected by user")));
    };

    TEST_CASE("shared_web_runtime_loads_skills_and_hooks") {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "skills-hooks-workspace";
        const auto skill_root = workspace / "skills";
        const auto hook_root = workspace / "hooks";
        std::filesystem::create_directories(skill_root / "web-runtime-skill");
        std::filesystem::create_directories(hook_root / "before_tool_call");

        {
            std::ofstream out(skill_root / "web-runtime-skill" / "SKILL.md");
            out << "---\n";
            out << "name: web-runtime-skill\n";
            out << "description: web runtime skill\n";
            out << "---\n\n";
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
        std::string session_id = "web-session-skills";

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, nullptr);

        CHECK(runtime.skills_prompt.contains("web-runtime-skill"));
        REQUIRE(runtime.hook_manager != nullptr);
        CHECK(runtime.hook_manager->hook_count(orangutan::HookEvent::before_tool_call) == 1);
    };

    TEST_CASE("automation_endpoints_expose_shared_state") {
        WebRoutesHarness harness;
        orangutan::bootstrap::AppRuntime app_runtime((harness.temp_root() / "automation.db"));
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
        CHECK_FALSE(task_id.empty());
        CHECK_FALSE(heartbeat_id.empty());
        CHECK_FALSE(inserted_inbox_id.empty());

        orangutan::WebServer server;
        server.set_automation_runtime(&automation_runtime);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto tasks_res = cli.Get("/api/tasks?agent_key=default");
        REQUIRE(static_cast<bool>(tasks_res));
        CHECK(tasks_res->status == 200);
        const auto tasks = nlohmann::json::parse(tasks_res->body);
        CHECK(tasks.size() == 1UL);
        CHECK(tasks[0]["name"] == "repo-check");
        CHECK(tasks[0]["schedule_kind"] == "cron");

        const auto heartbeats_res = cli.Get("/api/heartbeats?agent_key=default");
        REQUIRE(static_cast<bool>(heartbeats_res));
        CHECK(heartbeats_res->status == 200);
        const auto heartbeats = nlohmann::json::parse(heartbeats_res->body);
        CHECK(heartbeats.size() == 1UL);
        CHECK(heartbeats[0]["name"] == "self-check");

        const auto inbox_res = cli.Get("/api/inbox?agent_key=default");
        REQUIRE(static_cast<bool>(inbox_res));
        CHECK(inbox_res->status == 200);
        const auto inbox = nlohmann::json::parse(inbox_res->body);
        CHECK(inbox.size() == 1UL);
        const auto inbox_id = inbox[0]["id"].get<std::string>();
        CHECK(inbox[0]["title"] == "Task notification");

        const auto ack_res = cli.Post("/api/inbox/ack", nlohmann::json{{"agent_key", "default"}, {"id", inbox_id}}.dump(), "application/json");
        REQUIRE(static_cast<bool>(ack_res));
        CHECK(ack_res->status == 200);

        const auto inbox_after_ack = cli.Get("/api/inbox?agent_key=default");
        REQUIRE(static_cast<bool>(inbox_after_ack));
        CHECK(inbox_after_ack->status == 200);
        CHECK(nlohmann::json::parse(inbox_after_ack->body)[0]["status"] == "acked");

        const auto clear_res = cli.Delete("/api/inbox?agent_key=default");
        REQUIRE(static_cast<bool>(clear_res));
        CHECK(clear_res->status == 200);

        const auto inbox_after_clear = cli.Get("/api/inbox?agent_key=default");
        REQUIRE(static_cast<bool>(inbox_after_clear));
        CHECK(inbox_after_clear->status == 200);
        CHECK(nlohmann::json::parse(inbox_after_clear->body).empty());
        server.stop();
    };

    TEST_CASE("chat_approval_endpoint_rejects_invalid_approval_payload") {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;

        httplib::Request req;
        req.body = R"({"session_id":"web-session","approved":true})";
        httplib::Response res;

        orangutan::web::handle_chat_approval(req, res, sessions_mutex, sessions);

        CHECK(res.status == 400);
        CHECK(nlohmann::json::parse(res.body)["error"] == "missing or invalid 'request_id' field");
    };

    TEST_CASE("chat_approval_endpoint_approves_pending_request") {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
        auto session = std::make_unique<orangutan::web::WebSessionState>();
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
                *session_ptr, sessions_mutex, orangutan::ToolUse("shell-approval", "shell", nlohmann::json{{"command", "echo hello"}}),
                orangutan::PermissionDecision::ask_default("Shell command approval required."),
                [&](std::string_view current_event_name, const nlohmann::json &payload) {
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
            REQUIRE(event_cv.wait_for(lock, std::chrono::seconds(1), [&] {
                return event_payload.has_value();
            }));
        }

        REQUIRE(event_name.has_value());
        REQUIRE(event_payload.has_value());
        CHECK(*event_name == "approval_request");
        CHECK((*event_payload)["tool"] == "shell");
        CHECK((*event_payload)["command"] == "echo hello");

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

        CHECK(res.status == 200);
        CHECK(nlohmann::json::parse(res.body)["status"] == "approved");
        CHECK(approval_future.get());
        waiter.join();
    };

    TEST_CASE("chat_approval_endpoint_rejects_mismatched_request_id") {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
        auto session = std::make_unique<orangutan::web::WebSessionState>();
        session->session_id = "web-session";
        auto *session_ptr = session.get();
        sessions.emplace(session->session_id, std::move(session));

        std::promise<bool> approval_result;
        auto approval_future = approval_result.get_future();
        std::thread waiter([&] {
            approval_result.set_value(orangutan::web::detail::await_web_approval(
                *session_ptr, sessions_mutex, orangutan::ToolUse("shell-approval", "shell", nlohmann::json{{"command", "echo hello"}}),
                orangutan::PermissionDecision::ask_default("Shell command approval required."),
                [](std::string_view, const nlohmann::json &) {
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
            REQUIRE(session_ptr->pending_approval != nullptr);
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

        CHECK(res.status == 404);
        CHECK(nlohmann::json::parse(res.body)["error"] == "approval not found");

        orangutan::web::detail::cancel_pending_approval(*session_ptr);
        CHECK_FALSE(approval_future.get());
        waiter.join();
    };

} // namespace
