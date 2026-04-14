#include "web/web-server.hpp"
#include "web/web-routes.hpp"
#include "agent/agent-loop.hpp"
#include "automation/scheduler.hpp"
#include "automation/automation-store.hpp"
#include "bootstrap/identity.hpp"
#include "tools/background/background-completion.hpp"
#include "config/config.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "storage/session-store.hpp"
#include "test-helpers.hpp"
#include "test-provider-support.hpp"
#include "web/web-test-helpers.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

namespace orangutan {

    namespace {
        using orangutan::testing::web::make_profile;
        using orangutan::testing::web::make_session_metadata;

        Config make_config() {
            Config config;
            config.profile = "shared";
            config.model = "test";
            config.profiles.emplace("shared", make_profile({{"test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}},
                                                            {"coder-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
            config.agents["default"] = AgentConfig{
                .profile = "shared",
                .model = "test",
            };
            config.agents["coder"] = AgentConfig{
                .profile = "shared",
                .model = "coder-test",
            };
            return config;
        }

        const automation::InboxItem *find_inbox_item_by_body_type(const std::vector<automation::InboxItem> &items, const std::string &type) {
            const auto it = std::ranges::find_if(items, [&](const automation::InboxItem &item) {
                return nlohmann::json::parse(item.body).value("type", "") == type;
            });
            return it == items.end() ? nullptr : &(*it);
        }

        std::optional<nlohmann::json> find_sse_event_payload(std::string_view body, std::string_view event_name) {
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

            return nlohmann::json::parse(std::string(body.substr(payload_start, payload_end - payload_start)));
        }

        class ScriptedProvider {
        public:
            using Step = std::function<LLMResponse(const std::vector<Message> &)>;

            explicit ScriptedProvider(std::vector<Step> steps)
            : backend_(testing::make_fake_provider_backend([this](const providers::ProviderRoute &route, const providers::ProviderRequest &request,
                                                                  const providers::ProviderEventSink &) {
                  if (next_step_ >= steps_.size()) {
                      throw std::runtime_error("no scripted response available");
                  }
                  return providers::ProviderResult{
                      .response = steps_[next_step_++](request.messages),
                      .usage_snapshot = {},
                      .active_target = route.primary,
                  };
              })),
              steps_(std::move(steps)),
              system(backend_),
              route(testing::make_test_route("test")) {
                backend_->set_label("scripted-provider");
            }

            std::shared_ptr<testing::FakeProviderBackend> backend_;
            std::vector<Step> steps_;
            std::size_t next_step_ = 0;
            providers::ProviderSystem system;
            providers::ProviderRoute route;
        };

        class WebChatStoreHarness {
        public:
            WebChatStoreHarness()
            : db_path_(orangutan::testing::unique_test_db_path("web-chat", "sessions.db")),
              store_(db_path_.string()) {}

            ~WebChatStoreHarness() {
                std::filesystem::remove_all(db_path_.parent_path());
            }
            WebChatStoreHarness(const WebChatStoreHarness &) = delete;
            WebChatStoreHarness &operator=(const WebChatStoreHarness &) = delete;
            WebChatStoreHarness(WebChatStoreHarness &&) = delete;
            WebChatStoreHarness &operator=(WebChatStoreHarness &&) = delete;

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
            WebChatServerHarness(const WebChatServerHarness &) = delete;
            WebChatServerHarness &operator=(const WebChatServerHarness &) = delete;
            WebChatServerHarness(WebChatServerHarness &&) = delete;
            WebChatServerHarness &operator=(WebChatServerHarness &&) = delete;

            httplib::Client &client() {
                return *client_;
            }

        private:
            WebServer server_;
            std::unique_ptr<httplib::Client> client_;
        };

        TEST_CASE("chat_endpoint_rejects_missing_message") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/chat", "{}", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_rejects_invalid_json") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/chat", "not json", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_returns_503_without_config") {
            WebChatServerHarness harness;

            const auto res = harness.client().Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 503);
        };

        TEST_CASE("chat_endpoint_rejects_missing_agent_key") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/chat", R"({"message":"hello"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_rejects_missing_api_key") {
            orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
            orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
            const auto body = nlohmann::json::parse(res->body);
            CHECK(body["error"] == "missing api key for agent 'default'");
        };

        TEST_CASE("chat_handler_populates_completion_resume_state_for_active_session") {
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-active-session");
            config.agents["default"].workspace = workspace.string();

            const auto memory_db_path = orangutan::testing::unique_test_db_path("web-chat-active-session", "memory.db");
            MemoryStore memory_store(memory_db_path);
            WebChatStoreHarness harness;
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;

            httplib::Request req;
            req.body = R"({"message":"hello","agent_key":"default"})";
            httplib::Response res;

            orangutan::web::handle_chat(req, res, &config, &harness.store(), &memory_store, nullptr, nullptr, sessions_mutex, sessions);

            REQUIRE(sessions.size() == 1UL);
            const auto &session = *sessions.begin()->second;
            REQUIRE(session.completion_resume_state != nullptr);
            {
                std::scoped_lock lock(session.completion_resume_state->mutex);
                CHECK(session.completion_resume_state->agent != nullptr);
                CHECK(session.completion_resume_state->agent_key == "default");
                CHECK(session.completion_resume_state->automation_runtime == nullptr);
            }

            sessions.clear();
            std::filesystem::remove_all(memory_db_path.parent_path());
            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("help_slash_command_streams_help_without_api_key") {
            orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
            orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/chat", R"({"message":"/help","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text").get<std::string>().contains("/tasks run <id>"));
        };

        TEST_CASE("chat_endpoint_rejects_invalid_workspace_config") {
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

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 500);
            const auto body = nlohmann::json::parse(res->body);
            CHECK(body["error"].get<std::string>().contains("failed to resolve workspace for agent 'default'"));

            std::filesystem::remove(workspace_file);
        };

        TEST_CASE("chat_endpoint_rejects_read_only_channel_session") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            const auto session_id =
                store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/chat", nlohmann::json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 409);
        };

        TEST_CASE("chat_endpoint_rejects_cross_agent_session_access") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            const auto session_id =
                store_harness.store().save({Message::user().text("hello")}, make_session_metadata("coder-test", "agent:coder|web", "coder", "web", "web:local"));
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/chat", nlohmann::json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 404);
        };

        TEST_CASE("chat_handler_rejects_session_when_same_session_is_active") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            const auto session_id = store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));

            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;
            auto active = std::make_unique<web::WebSessionState>();
            active->session_id = session_id;
            sessions.emplace(session_id, std::move(active));

            httplib::Request req;
            req.body = nlohmann::json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump();
            httplib::Response res;

            web::handle_chat(req, res, &config, &store_harness.store(), nullptr, nullptr, nullptr, sessions_mutex, sessions);

            CHECK(res.status == 409);
            REQUIRE_FALSE(res.body.empty());
            CHECK(nlohmann::json::parse(res.body).at("error") == "session already active");
        };

        TEST_CASE("chat_handler_uses_shared_runtime_guard_for_shell_path_checks") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-guard-workspace");
            config.agents["default"].workspace = workspace.string();
            const auto session_id = store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));

            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;

            httplib::Request req;
            req.body = nlohmann::json{{"message", "run shell"}, {"agent_key", "default"}, {"session_id", session_id}}.dump();
            httplib::Response res;

            web::handle_chat(req, res, &config, &store_harness.store(), nullptr, nullptr, nullptr, sessions_mutex, sessions);

            std::scoped_lock lock(sessions_mutex);
            REQUIRE(sessions.contains(session_id));
            auto &session = sessions.at(session_id);
            REQUIRE(session != nullptr);
            REQUIRE(session->runtime != nullptr);

            session->runtime->tool_context().approval_callback = [](const ToolUse &, const PermissionDecision &) {
                return false;
            };
            const auto result = session->runtime->tools().execute(ToolUse("shell-check", "shell", {{"command", "cat /etc/passwd"}}));
            CHECK(result.is_error);
            CHECK(result.content.contains("permission scope"));
        };

        TEST_CASE("resume_slash_command_streams_target_session_event") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            const auto session_id = store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/chat", nlohmann::json{{"message", std::string("/resume ") + session_id}, {"agent_key", "default"}}.dump(), "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto session_event = find_sse_event_payload(res->body, "session");
            REQUIRE(session_event.has_value());
            CHECK(session_event->at("session_id") == session_id);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text").get<std::string>().contains(session_id));
        };

        TEST_CASE("new_slash_command_streams_markdown_reply_and_session_event") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            const auto existing_session_id =
                store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|web", "default", "web", "web:local"));
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/chat", nlohmann::json{{"message", "/new"}, {"agent_key", "default"}, {"session_id", existing_session_id}}.dump(), "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto session_event = find_sse_event_payload(res->body, "session");
            REQUIRE(session_event.has_value());
            CHECK(session_event->at("session_id") != existing_session_id);
            CHECK_FALSE(find_sse_event_payload(res->body, "text").has_value());
        };

        TEST_CASE("export_slash_command_works_for_read_only_channel_session") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-export");
            config.agents["default"].workspace = workspace.string();

            const auto session_id = store_harness.store().save({Message::user().text("hello"), Message::assistant().text("copied reply")},
                                                               make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
            const auto export_path = orangutan::bootstrap::workspace_exports_root(workspace.string()) / (session_id + ".md");
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/chat", nlohmann::json{{"message", "/export"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text") == "## Export\n- Saved current session to `" + export_path.string() + '`');
            REQUIRE(std::filesystem::exists(export_path));

            std::ifstream in(export_path);
            const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(content.contains("# Session Export"));
            CHECK(content.contains("hello"));
            CHECK(content.contains("copied reply"));

            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("abort_endpoint_returns_404_for_unknown_session") {
            WebChatServerHarness harness;

            const auto res = harness.client().Post("/api/chat/abort", R"({"session_id":"nonexistent"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 404);
        };

        TEST_CASE("abort_endpoint_rejects_missing_session_id") {
            WebChatServerHarness harness;

            const auto res = harness.client().Post("/api/chat/abort", "{}", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("runtime_bundle_includes_custom_and_memory_tools") {
            const auto workspace = orangutan::testing::unique_test_root("web-chat-runtime-workspace");

            auto config = make_config();
            config.custom_tools.push_back(Config::ScriptToolConfig{
                .name = "custom_echo",
                .description = "Custom echo script tool",
                .command = "echo hello",
            });
            config.agents["default"].workspace = workspace.string();
            config.agents["default"].team_agents = {"coder"};
            config.agents["default"].permissions_config = {};
            config.agents["coder"].workspace = workspace.string();

            MemoryStore memory_store((workspace / "memory.db"));
            std::string session_id = "web-chat-runtime-session";

            auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, nullptr, [](const ToolUse &, const PermissionDecision &) {
                return false;
            });

            CHECK(not(orangutan::testing::has_tool_named(runtime.tools().definitions(), "memory_list")));
            CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "custom_echo"));
            CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "tool_search"));
            CHECK(runtime.tool_context().runtime_origin == base::origin::web);
            CHECK(runtime.tool_context().raw_caller_id == "web:local");
            CHECK(runtime.tool_context().current_session_id == &session_id);
            CHECK(runtime.tool_context().team_agents == std::vector<std::string>{"coder"});
            CHECK(static_cast<bool>(runtime.tool_context().approval_callback));
            REQUIRE(runtime.agent != nullptr);

            const auto shell_result = runtime.tools().execute(ToolUse("web-shell", "shell", {{"command", "echo hello"}}));
            CHECK(shell_result.is_error);
            CHECK((shell_result.content.contains("Requires approval") || shell_result.content.contains("Rejected by user")));
        };

        TEST_CASE("tasks_slash_command_uses_runtime_tool_output") {
            Config config = make_config();
            auto automation_store = std::make_shared<automation::Store>(orangutan::testing::unique_test_db_path("web-chat-tasks", "automation.db"));
            automation::Runtime automation_runtime(*automation_store);
            WebChatServerHarness harness(&config, nullptr, &automation_runtime);

            const auto res = harness.client().Post("/api/chat", R"({"message":"/tasks","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text") == "## Tasks\n- 🗓️ No tasks configured.");
        };

        TEST_CASE("runtime_bundle_loads_skills_and_hooks_from_configured_paths") {
            const auto workspace = orangutan::testing::unique_test_root("web-chat-runtime-skills-hooks");
            const auto skill_root = workspace / "skills";
            const auto hook_root = workspace / "hooks";
            std::filesystem::create_directories(skill_root / "web-chat-runtime-skill");
            std::filesystem::create_directories(hook_root / "before_tool_call");

            {
                std::ofstream out(skill_root / "web-chat-runtime-skill" / "SKILL.md");
                out << "---\n";
                out << "name: web-chat-runtime-skill\n";
                out << "description: web chat runtime skill\n";
                out << "---\n\n";
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
            std::string session_id = "web-chat-skills-hooks";

            auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, nullptr);

            CHECK(runtime.skills_prompt.contains("web-chat-runtime-skill"));
            REQUIRE(runtime.hook_manager != nullptr);
            CHECK(runtime.hook_manager->hook_count(hook_event::before_tool_call) == 1);
        };

        TEST_CASE("completion_resume_after_session_shutdown_falls_back_to_inbox_notes") {
            const auto automation_db_path = orangutan::testing::unique_test_db_path("web-chat-automation", "automation.db");
            auto automation_store = std::make_shared<automation::Store>(automation_db_path);
            automation::Runtime automation_runtime(*automation_store);
            ToolRegistry tools;
            std::size_t provider_calls = 0;
            ScriptedProvider provider({
                [&provider_calls](const std::vector<Message> &) {
                    ++provider_calls;
                    LLMResponse response;
                    response.stop_reason = response_stop_reason::end_turn;
                    response.content.emplace_back(Text{"should not run"});
                    return response;
                },
            });
            AgentLoop agent(provider.system, provider.route, tools);

            auto resume_state = std::make_shared<orangutan::web::WebCompletionResumeState>();
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
            orangutan::tools::BackgroundCompletionDispatcher dispatcher(&tool_context);

            dispatcher.dispatch(BackgroundProcessCompletionEvent{
                .process_id = "proc-web",
                .command = "sleep 1",
                .working_dir = orangutan::testing::unique_test_root("web-chat-completion").string(),
                .pid = 1234,
                .terminal_status = background_process_terminal_status::exited,
                .exit_code = 0,
                .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
                .metadata = {{std::string(orangutan::tools::BACKGROUND_COMPLETION_MODE_METADATA_KEY), "resume"}},
            });

            CHECK(provider_calls == 0UL);
            const auto inbox_items = automation_runtime.list_inbox("default");
            REQUIRE(inbox_items.size() == 2UL);

            const auto *completion_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion");
            const auto *failure_item = find_inbox_item_by_body_type(inbox_items, "background_process_completion_resume_failure");
            REQUIRE(completion_item != nullptr);
            REQUIRE(failure_item != nullptr);
            CHECK(nlohmann::json::parse(failure_item->body).at("reason") == "web session is no longer live");
            std::filesystem::remove(automation_db_path);
        };

        TEST_CASE("pending_approval_denial_returns_false") {
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
            auto session = std::make_unique<orangutan::web::WebSessionState>();
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
                    *session_ptr, sessions_mutex, ToolUse("shell-deny", "shell", nlohmann::json{{"command", "echo hello"}}),
                    PermissionDecision::ask_default("Shell command approval required."),
                    [&](std::string_view, const nlohmann::json &payload) {
                        std::scoped_lock lock(event_mutex);
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
            REQUIRE(event_payload.has_value());

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

            CHECK(res.status == 200);
            CHECK(nlohmann::json::parse(res.body)["status"] == "denied");
            CHECK_FALSE(approval_future.get());
            waiter.join();
        };

        TEST_CASE("abort_endpoint_cancels_pending_approval") {
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
            auto session = std::make_unique<orangutan::web::WebSessionState>();
            session->session_id = "web-session";
            auto *session_ptr = session.get();
            sessions.emplace(session->session_id, std::move(session));

            std::promise<bool> approval_result;
            auto approval_future = approval_result.get_future();
            std::thread waiter([&] {
                approval_result.set_value(web::detail::await_web_approval(
                    *session_ptr, sessions_mutex, ToolUse("shell-abort", "shell", nlohmann::json{{"command", "echo hello"}}),
                    PermissionDecision::ask_default("Shell command approval required."),
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
            req.body = R"({"session_id":"web-session"})";
            httplib::Response res;
            web::handle_chat_abort(req, res, sessions_mutex, sessions);

            CHECK(nlohmann::json::parse(res.body)["status"] == "abort_requested");
            CHECK(session_ptr->abort_requested.load());
            CHECK_FALSE(approval_future.get());
            waiter.join();
        };

        TEST_CASE("pending_approval_timeout_behaves_as_denied") {
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
            auto session = std::make_unique<orangutan::web::WebSessionState>();
            session->session_id = "web-session";
            auto *session_ptr = session.get();
            sessions.emplace(session->session_id, std::move(session));

            const auto approved = web::detail::await_web_approval(
                *session_ptr, sessions_mutex, ToolUse("shell-timeout", "shell", nlohmann::json{{"command", "echo hello"}}),
                PermissionDecision::ask_default("Shell command approval required."),
                [](std::string_view, const nlohmann::json &) {
                    return true;
                },
                {}, std::chrono::milliseconds(50));

            CHECK_FALSE(approved);
            std::scoped_lock lock(sessions_mutex);
            CHECK(session_ptr->pending_approval == nullptr);
        };

        TEST_CASE("pending_approval_cleanup_cancels_unresolved_request") {
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<orangutan::web::WebSessionState>> sessions;
            auto session = std::make_unique<orangutan::web::WebSessionState>();
            session->session_id = "web-session";
            auto *session_ptr = session.get();
            sessions.emplace(session->session_id, std::move(session));

            std::promise<bool> approval_result;
            auto approval_future = approval_result.get_future();
            std::thread waiter([&] {
                approval_result.set_value(web::detail::await_web_approval(
                    *session_ptr, sessions_mutex, ToolUse("shell-cleanup", "shell", nlohmann::json{{"command", "echo hello"}}),
                    PermissionDecision::ask_default("Shell command approval required."),
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

            web::detail::cancel_pending_approval(*session_ptr);

            CHECK_FALSE(approval_future.get());
            waiter.join();
        };

    } // namespace

} // namespace orangutan
