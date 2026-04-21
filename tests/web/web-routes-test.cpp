#include "web/web-server.hpp"
#include "web/web-routes.hpp"
#include "bootstrap/app-runtime.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "storage/session-store.hpp"
#include "config/config.hpp"
#include "test-helpers.hpp"
#include "web/web-test-helpers.hpp"
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

    using orangutan::testing::web::make_profile;
    using orangutan::testing::web::make_session_metadata;

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

    orangutan::Config make_runtime_config(const std::filesystem::path &workspace_root) {
        orangutan::Config cfg;
        cfg.profile = "shared";
        cfg.model = "gpt-test";
        cfg.profiles.emplace("shared", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}},
                                                     {"gpt-coder-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
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
                    .default_mode = orangutan::permission_mode::default_mode,
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

    [[nodiscard]]
    orangutan::web::WebContext make_web_context(std::mutex &sessions_mutex,
                                                std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> &sessions) {
        return orangutan::web::WebContext{
            .sessions_mutex = &sessions_mutex,
            .sessions = &sessions,
        };
    }

    TEST_CASE("list_sessions_returns_empty_array") {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/sessions");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["items"].is_array());
        CHECK(body["items"].empty());
        server.stop();
    };

    TEST_CASE("get_session_returns_404_for_missing") {
        WebRoutesHarness harness;
        orangutan::WebServer server;
        server.set_session_store(harness.session_store());
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/sessions/nonexistent");
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
        const auto res = cli.Delete("/api/v1/sessions/nonexistent");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        server.stop();
    };

    TEST_CASE("get_config_returns_json") {
        orangutan::Config cfg;
        cfg.model = "test-model";
        cfg.permissions_config = orangutan::PermissionConfig{
            .default_mode = orangutan::permission_mode::accept_edits,
            .allow = {"read"},
            .ask = {"shell"},
        };
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/config");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["agent"]["model"] == "test-model");
        CHECK(body["permissions"]["default_mode"] == "accept_edits");
        CHECK(body["permissions"]["allow"] == nlohmann::json::array({"read"}));
        CHECK(body["permissions"]["ask"] == nlohmann::json::array({"shell"}));
        CHECK_FALSE(body["tools"].contains("allowed"));
        CHECK_FALSE(body.contains("api_key"));
        server.stop();
    };

    TEST_CASE("get_config_returns_503_when_null") {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/config");
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
        const auto res = cli.Put("/api/v1/config", R"({"agent":{"model":"new-model"}})", "application/json");
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
        const auto res = cli.Get("/api/v1/tools");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 503);
        server.stop();
    };

    TEST_CASE("list_agents_returns_array") {
        orangutan::Config cfg;
        cfg.profile = "shared";
        cfg.model = "default-model";
        cfg.profiles.emplace("shared", make_profile({{"default-model", ModelConfig{.provider = "openai", .protocol = "chat-completions"}},
                                                     {"helper-model", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
        cfg.agents["default"] = orangutan::AgentConfig{.profile = "shared", .model = "default-model", .team_agents = {"helper"}};
        cfg.agents["helper"] = orangutan::AgentConfig{.profile = "shared", .model = "helper-model"};
        orangutan::WebServer server;
        server.set_config(&cfg);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/agents");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["items"].is_array());
        const auto &agents = body["items"];
        CHECK(agents.size() == 2UL);
        bool saw_default = false;
        bool saw_helper = false;
        for (const auto &agent : agents) {
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
        cfg.profiles.emplace("shared", make_profile({{"default-model", ModelConfig{.provider = "openai", .protocol = "chat-completions"}},
                                                     {"coder-model", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
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

        const auto res = cli.Get("/api/v1/agents/coder/sessions");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["items"].is_array());
        const auto &sessions_body = body["items"];
        CHECK(sessions_body.size() == 2UL);
        CHECK(sessions_body[0]["agent_key"] == "coder");
        CHECK(sessions_body[1]["agent_key"] == "coder");
        CHECK((sessions_body[0]["read_only"].get<bool>() || sessions_body[1]["read_only"].get<bool>()));
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

        const auto res = cli.Get("/api/v1/agents/default/sessions/" + coder_session_id);
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 404);
        server.stop();
    };

    TEST_CASE("system_status_returns_uptime") {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/v1/system/status");
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

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, &app_runtime.automation_service(),
                                                                        &app_runtime.automation_runtime(),
                                                                        [](const orangutan::ToolUse &, const orangutan::PermissionDecision &) {
                                                                            return false;
                                                                        });

        const auto definitions = runtime.tools().definitions();
        CHECK(not(orangutan::testing::has_tool_named(definitions, "memory_list")));
        CHECK(orangutan::testing::has_tool_named(definitions, "shell"));
        CHECK(orangutan::testing::has_tool_named(definitions, "custom_echo"));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "task")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "heartbeat")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "inbox")));
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(runtime.tool_context().runtime_origin == base::origin::web);
        CHECK(runtime.tool_context().raw_caller_id == "web:local");
        CHECK(runtime.tool_context().current_session_id == &session_id);
        CHECK(runtime.tool_context().team_agents == std::vector<std::string>({"coder"}));
        CHECK(runtime.tool_context().automation_service == &app_runtime.automation_service());
        CHECK(runtime.tool_context().automation_runtime == &app_runtime.automation_runtime());
        CHECK(runtime.tool_context().approval_callback != nullptr);
        CHECK(runtime.agent != nullptr);

        const auto shell = std::ranges::find_if(definitions, [](const orangutan::ToolDef &definition) {
            return definition.name == "shell";
        });
        REQUIRE(shell != definitions.end());
        CHECK(shell->input_schema.contains("properties"));
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));

        const auto shell_result = runtime.tools().execute(orangutan::ToolUse("web-shell", "shell", {{"command", "echo hello"}}));
        CHECK(shell_result.is_error);
        CHECK((shell_result.content.contains("Requires approval") || shell_result.content.contains("Rejected by user")));
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

        auto runtime = orangutan::web::detail::build_web_runtime_bundle(cfg, "default", &memory_store, &session_id, nullptr, nullptr);

        CHECK(runtime.skills_prompt.contains("web-runtime-skill"));
        REQUIRE(runtime.hook_manager != nullptr);
        CHECK(runtime.hook_manager->hook_count(orangutan::hook_event::before_tool_call) == 1);
    };

    TEST_CASE("list_skills_endpoint_returns_schema_v2_payload") {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "skills-endpoint-workspace";
        const auto skill_root = workspace / "skills";
        std::filesystem::create_directories(skill_root / "endpoint-skill");

        {
            std::ofstream out(skill_root / "endpoint-skill" / "SKILL.md");
            out << "---\n";
            out << "name: endpoint-skill\n";
            out << "description: endpoint skill description\n";
            out << "tools: [read]\n";
            out << "---\n\n";
            out << "endpoint skill body\n";
        }

        orangutan::SkillLoader loader;
        loader.load_from_directories({skill_root});

        orangutan::WebServer server;
        server.set_skill_loader(&loader);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/v1/skills");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);

        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["schema_version"] == 2);
        REQUIRE(body["items"].is_array());
        REQUIRE(body["items"].size() == 1UL);
        CHECK(body["items"][0]["name"] == "endpoint-skill");
        CHECK(body["items"][0]["scope"] == "always");
        CHECK(body["items"][0]["active"] == true);
        CHECK(body["items"][0]["diagnostic_count"] == 0);
        CHECK(body["items"][0].contains("id"));
        CHECK(body["items"][0].contains("source"));
        CHECK(body["items"][0].contains("source_path"));

        server.stop();
    };

    TEST_CASE("list_skills_endpoint_includes_inactive_conditional_skills") {
        WebRoutesHarness harness;
        const auto workspace = harness.temp_root() / "skills-endpoint-conditional-workspace";
        const auto skill_root = workspace / "skills";
        std::filesystem::create_directories(skill_root / "always-skill");
        std::filesystem::create_directories(skill_root / "conditional-skill");

        {
            std::ofstream out(skill_root / "always-skill" / "SKILL.md");
            out << "---\n";
            out << "name: always-skill\n";
            out << "description: always skill\n";
            out << "---\n\n";
            out << "always body\n";
        }
        {
            std::ofstream out(skill_root / "conditional-skill" / "SKILL.md");
            out << "---\n";
            out << "name: conditional-skill\n";
            out << "description: conditional skill\n";
            out << "scope: conditional\n";
            out << "paths_any: [src/*.cpp]\n";
            out << "---\n\n";
            out << "conditional body\n";
        }

        orangutan::SkillLoader loader;
        loader.load_from_directories({skill_root});

        orangutan::WebServer server;
        server.set_skill_loader(&loader);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto res = cli.Get("/api/v1/skills");
        REQUIRE(static_cast<bool>(res));
        CHECK(res->status == 200);

        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["schema_version"] == 2);
        REQUIRE(body["items"].is_array());
        REQUIRE(body["items"].size() == 2UL);

        const auto conditional = std::ranges::find_if(body["items"], [](const nlohmann::json &skill) {
            return skill["name"] == "conditional-skill";
        });
        REQUIRE(conditional != body["items"].end());
        CHECK((*conditional)["scope"] == "conditional");
        CHECK((*conditional)["active"] == false);

        server.stop();
    };

    TEST_CASE("automation_endpoints_expose_shared_state") {
        WebRoutesHarness harness;
        orangutan::bootstrap::AppRuntime app_runtime((harness.temp_root() / "automation.db"));
        auto &automation_service = app_runtime.automation_service();
        auto &automation_runtime = app_runtime.automation_runtime();

        orangutan::WebServer server;
        server.set_automation_service(&automation_service);
        server.set_automation_runtime(&automation_runtime);
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());

        const auto create_res = cli.Post("/api/v1/automation",
                                         nlohmann::json{
                                             {"agent_key", "default"},
                                             {"name", "repo-check"},
                                             {"prompt", "Check the repository state."},
                                             {"trigger", {{"type", "cron"}, {"cron", "0 * * * *"}}},
                                             {"delivery", {{"mode", "notify"}, {"targets", {"owner"}}}},
                                         }
                                             .dump(),
                                         "application/json");
        REQUIRE(static_cast<bool>(create_res));
        CHECK(create_res->status == 201);
        const auto created = nlohmann::json::parse(create_res->body);
        const auto automation_id = created.at("id").get<std::string>();
        CHECK(created.at("name") == "repo-check");

        const auto list_res = cli.Get("/api/v1/automation?agent_key=default");
        REQUIRE(static_cast<bool>(list_res));
        CHECK(list_res->status == 200);
        const auto listed = nlohmann::json::parse(list_res->body);
        REQUIRE(listed.at("items").size() == 1UL);
        CHECK(listed.at("items").at(0).at("id") == automation_id);

        const auto get_res = cli.Get("/api/v1/automation/" + automation_id + "?agent_key=default");
        REQUIRE(static_cast<bool>(get_res));
        CHECK(get_res->status == 200);
        CHECK(nlohmann::json::parse(get_res->body).at("prompt") == "Check the repository state.");

        const auto patch_silent_res = cli.Patch("/api/v1/automation/" + automation_id + "?agent_key=default",
                                                nlohmann::json{{"delivery", {{"mode", "silent"}, {"targets", nlohmann::json::array()}}}}.dump(),
                                                "application/json");
        REQUIRE(static_cast<bool>(patch_silent_res));
        CHECK(patch_silent_res->status == 200);
        CHECK(nlohmann::json::parse(patch_silent_res->body).at("delivery").at("mode") == "silent");

        const auto patch_notify_res = cli.Patch("/api/v1/automation/" + automation_id + "?agent_key=default",
                                                nlohmann::json{{"delivery", {{"mode", "notify"}, {"targets", {"owner"}}}}}.dump(),
                                                "application/json");
        REQUIRE(static_cast<bool>(patch_notify_res));
        CHECK(patch_notify_res->status == 200);

        const auto run_res = cli.Post("/api/v1/automation/" + automation_id + "/run?agent_key=default", "", "application/json");
        REQUIRE(static_cast<bool>(run_res));
        CHECK(run_res->status == 200);
        CHECK_FALSE(nlohmann::json::parse(run_res->body).at("run_id").get<std::string>().empty());

        const auto runs_res = cli.Get("/api/v1/automation/runs?agent_key=default&automation_id=" + automation_id);
        REQUIRE(static_cast<bool>(runs_res));
        CHECK(runs_res->status == 200);
        const auto runs = nlohmann::json::parse(runs_res->body);
        REQUIRE(runs.at("items").is_array());
        CHECK(runs.at("items").size() == 1UL);

        const auto deliveries_res = cli.Get("/api/v1/automation/deliveries?agent_key=default&automation_id=" + automation_id);
        REQUIRE(static_cast<bool>(deliveries_res));
        CHECK(deliveries_res->status == 200);
        const auto deliveries = nlohmann::json::parse(deliveries_res->body);
        REQUIRE(deliveries.at("items").is_array());
        REQUIRE(deliveries.at("items").size() == 1UL);
        const auto delivery_id = deliveries.at("items").at(0).at("id").get<std::string>();

        const auto ack_res = cli.Post("/api/v1/automation/deliveries/" + delivery_id + "/ack?agent_key=default", "", "application/json");
        REQUIRE(static_cast<bool>(ack_res));
        CHECK(ack_res->status == 200);

        const auto clear_res = cli.Delete("/api/v1/automation/deliveries?agent_key=default&automation_id=" + automation_id);
        REQUIRE(static_cast<bool>(clear_res));
        CHECK(clear_res->status == 200);

        const auto deliveries_after_clear = cli.Get("/api/v1/automation/deliveries?agent_key=default&automation_id=" + automation_id + "&only_unacked=true");
        REQUIRE(static_cast<bool>(deliveries_after_clear));
        CHECK(deliveries_after_clear->status == 200);
        CHECK(nlohmann::json::parse(deliveries_after_clear->body).at("items").empty());

        const auto delete_res = cli.Delete("/api/v1/automation/" + automation_id + "?agent_key=default");
        REQUIRE(static_cast<bool>(delete_res));
        CHECK(delete_res->status == 200);

        server.stop();
    };

    TEST_CASE("chat_approval_endpoint_rejects_invalid_approval_payload") {
        std::mutex sessions_mutex;
        std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;

        httplib::Request req;
        req.body = R"({"session_id":"web-session","approved":true})";
        httplib::Response res;

        const auto ctx = make_web_context(sessions_mutex, sessions);
        orangutan::web::handle_chat_approval(ctx, req, res);

        CHECK(res.status == 400);
        CHECK(nlohmann::json::parse(res.body)["error"]["message"] == "missing or invalid 'request_id' field");
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
                orangutan::PermissionDecision::ask_by_rule(orangutan::permission_rule_source::project_settings, "shell(echo hello)", "Shell command approval required."),
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
        CHECK((*event_payload)["decision"]["behavior"] == "ask");
        CHECK((*event_payload)["decision"]["reason"]["type"] == "rule");
        CHECK((*event_payload)["decision"]["reason"]["source"] == "project_settings");
        CHECK((*event_payload)["decision"]["reason"]["rule_value"] == "shell(echo hello)");

        httplib::Request req;
        req.body =
            nlohmann::json{
                {"session_id", "web-session"},
                {"request_id", (*event_payload)["request_id"]},
                {"approved", true},
            }
                .dump();
        httplib::Response res;

        const auto ctx = make_web_context(sessions_mutex, sessions);
        orangutan::web::handle_chat_approval(ctx, req, res);

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

        const auto ctx = make_web_context(sessions_mutex, sessions);
        orangutan::web::handle_chat_approval(ctx, req, res);

        CHECK(res.status == 404);
        CHECK(nlohmann::json::parse(res.body)["error"]["message"] == "approval not found");

        orangutan::web::detail::cancel_pending_approval(*session_ptr);
        CHECK_FALSE(approval_future.get());
        waiter.join();
    };

} // namespace
