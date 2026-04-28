#include "web/web-server.hpp"
#include "web/web-routes.hpp"
#include "agent/agent-loop.hpp"
#include "bootstrap/app-runtime.hpp"
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
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <new>
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <vector>

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

        const automation::DeliveryRecord *find_delivery_by_body_type(const std::vector<automation::DeliveryRecord> &deliveries, const std::string &type) {
            const auto it = std::ranges::find_if(deliveries, [&](const automation::DeliveryRecord &delivery) {
                return nlohmann::json::parse(delivery.body).value("type", "") == type;
            });
            return it == deliveries.end() ? nullptr : &(*it);
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

        std::vector<std::string> sse_event_names(std::string_view body) {
            std::vector<std::string> names;
            constexpr std::string_view marker = "event: ";
            std::size_t pos = 0;
            while (pos < body.size()) {
                const auto event_start = body.find(marker, pos);
                if (event_start == std::string_view::npos) {
                    break;
                }
                const auto name_start = event_start + marker.size();
                const auto name_end = body.find('\n', name_start);
                if (name_end == std::string_view::npos) {
                    break;
                }
                names.emplace_back(body.substr(name_start, name_end - name_start));
                pos = name_end + 1;
            }
            return names;
        }

        std::string provider_sse_data(const nlohmann::json &payload) {
            return "data: " + payload.dump() + "\n\n";
        }

        std::string openai_text_stream(std::string text) {
            return provider_sse_data({{"choices", nlohmann::json::array({{{"delta", {{"content", std::move(text)}}}, {"finish_reason", nullptr}}})}}) +
                   provider_sse_data({{"choices", nlohmann::json::array({{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}})}}) + "data: [DONE]\n\n";
        }

        std::string openai_shell_tool_stream() {
            nlohmann::json first_delta;
            first_delta["tool_calls"] = nlohmann::json::array({{
                {"index", 0},
                {"id", "call-shell"},
                {"function", {{"name", "shell"}, {"arguments", R"({"command":"echo hello"})"}}},
            }});
            return provider_sse_data({{"choices", nlohmann::json::array({{{"delta", std::move(first_delta)}, {"finish_reason", nullptr}}})}}) +
                   provider_sse_data({{"choices", nlohmann::json::array({{{"delta", nlohmann::json::object()}, {"finish_reason", "tool_calls"}}})}}) + "data: [DONE]\n\n";
        }

        std::string drain_response_stream(httplib::Response &res, const std::function<void(std::string_view)> &on_write = {}, const std::function<bool()> &is_writable = {}) {
            REQUIRE(static_cast<bool>(res.content_provider_));
            std::string body;
            bool done_called = false;
            httplib::DataSink sink;
            sink.write = [&](const char *data, std::size_t data_len) {
                const auto chunk = std::string_view(data, data_len);
                body.append(chunk);
                if (on_write != nullptr) {
                    on_write(chunk);
                }
                return true;
            };
            sink.is_writable = is_writable != nullptr ? is_writable : std::function<bool()>{[] {
                return true;
            }};
            sink.done = [&] {
                done_called = true;
            };

            const bool keep_open = res.content_provider_(0, 0, sink);

            CHECK_FALSE(keep_open);
            CHECK(done_called);
            return body;
        }

        [[nodiscard]]
        web::WebContext make_web_context(Config *config, SessionStore *session_store, MemoryStore *memory_store, std::mutex &sessions_mutex,
                                         std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> &sessions) {
            return web::WebContext{
                .config = config,
                .session_store = session_store,
                .memory_store = memory_store,
                .sessions_mutex = &sessions_mutex,
                .sessions = &sessions,
            };
        }

        class ScriptedProvider {
        public:
            using Step = std::function<LLMResponse(const std::vector<Message> &)>;

            explicit ScriptedProvider(std::vector<Step> steps)
            : backend_(testing::make_fake_provider_backend(
                  [this](const providers::ProviderRoute &route, const providers::ProviderRequest &request, const providers::ProviderEventSink &) {
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
            WebChatServerHarness(Config *config = nullptr, SessionStore *store = nullptr, automation::AutomationRuntime *automation_runtime = nullptr) {
                if (config != nullptr) {
                    server_.set_config(config);
                }
                if (store != nullptr) {
                    server_.set_session_store(store);
                }
                if (automation_runtime != nullptr) {
                    server_.set_automation_service(&automation_runtime->service());
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

        struct ProviderHttpResponse {
            int status = 200;
            std::string body;
        };

        class StreamingOpenAiServer {
        public:
            explicit StreamingOpenAiServer(std::vector<ProviderHttpResponse> responses)
            : responses_(std::move(responses)) {
                server_.Post("/v1/chat/completions", [this](const httplib::Request &, httplib::Response &response) {
                    ProviderHttpResponse scripted;
                    {
                        std::scoped_lock lock(mutex_);
                        if (responses_.empty()) {
                            scripted = ProviderHttpResponse{.status = 500, .body = R"({"error":"no scripted response"})"};
                        } else if (next_response_ < responses_.size()) {
                            scripted = responses_[next_response_++];
                        } else {
                            scripted = responses_.back();
                        }
                    }

                    response.status = scripted.status;
                    response.set_content(std::move(scripted.body), scripted.status >= 200 && scripted.status < 300 ? "text/event-stream" : "application/json");
                });
                port_ = server_.bind_to_any_port("127.0.0.1");
                if (port_ <= 0) {
                    throw std::runtime_error("failed to bind streaming provider server");
                }
                server_thread_ = std::jthread([this] {
                    server_.listen_after_bind();
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!server_.is_running() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                }
                if (!server_.is_running()) {
                    server_.stop();
                    throw std::runtime_error("streaming provider server did not start");
                }
            }

            ~StreamingOpenAiServer() {
                server_.stop();
                if (server_thread_.joinable()) {
                    server_thread_.join();
                }
            }
            StreamingOpenAiServer(const StreamingOpenAiServer &) = delete;
            StreamingOpenAiServer &operator=(const StreamingOpenAiServer &) = delete;
            StreamingOpenAiServer(StreamingOpenAiServer &&) = delete;
            StreamingOpenAiServer &operator=(StreamingOpenAiServer &&) = delete;

            [[nodiscard]]
            std::string base_url() const {
                return "http://127.0.0.1:" + std::to_string(port_);
            }

        private:
            httplib::Server server_;
            std::vector<ProviderHttpResponse> responses_;
            std::size_t next_response_ = 0;
            int port_ = 0;
            std::mutex mutex_;
            std::jthread server_thread_;
        };

        TEST_CASE("chat_endpoint_rejects_missing_message") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/v1/chat", "{}", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_rejects_invalid_json") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/v1/chat", "not json", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_returns_503_without_config") {
            WebChatServerHarness harness;

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 503);
        };

        TEST_CASE("chat_endpoint_rejects_missing_agent_key") {
            Config config = make_config();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"hello"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
        };

        TEST_CASE("chat_endpoint_rejects_missing_api_key") {
            orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
            orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 400);
            const auto body = nlohmann::json::parse(res->body);
            CHECK(body["error"]["message"] == "missing api key for agent 'default'");
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

            const auto ctx = make_web_context(&config, &harness.store(), &memory_store, sessions_mutex, sessions);
            orangutan::web::handle_chat(ctx, req, res);

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

        TEST_CASE("chat_handler_streaming_success_removes_active_session_and_preserves_event_order") {
            StreamingOpenAiServer provider({ProviderHttpResponse{.body = openai_text_stream("streamed reply")}});
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-stream-success");
            config.profiles.at("shared").base_url = provider.base_url();
            config.agents["default"].workspace = workspace.string();
            WebChatStoreHarness store_harness;
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;
            web::EventBus event_bus;
            std::vector<std::string> bus_events;
            auto subscription = event_bus.subscribe([&bus_events](const web::BusEvent &event) {
                bus_events.push_back(event.kind);
            });

            httplib::Request req;
            req.body = R"({"message":"hello","agent_key":"default"})";
            httplib::Response res;
            auto ctx = make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions);
            ctx.event_bus = &event_bus;

            web::handle_chat(ctx, req, res);

            REQUIRE(sessions.size() == 1UL);
            const auto body = drain_response_stream(res);

            CHECK(sessions.empty());
            CHECK(sse_event_names(body) == std::vector<std::string>{"session", "text", "done"});
            const auto text_event = find_sse_event_payload(body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text") == "streamed reply");
            CHECK(bus_events == std::vector<std::string>{"chat.session_started", "chat.text", "chat.done"});

            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("chat_handler_stream_callbacks_do_not_depend_on_web_context_object_lifetime") {
            StreamingOpenAiServer provider({ProviderHttpResponse{.body = openai_text_stream("streamed reply")}});
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-context-lifetime");
            config.profiles.at("shared").base_url = provider.base_url();
            config.agents["default"].workspace = workspace.string();
            WebChatStoreHarness store_harness;
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;
            web::EventBus event_bus;
            std::vector<std::string> bus_events;
            auto subscription = event_bus.subscribe([&bus_events](const web::BusEvent &event) {
                bus_events.push_back(event.kind);
            });

            httplib::Request req;
            req.body = R"({"message":"hello","agent_key":"default"})";
            httplib::Response res;

            alignas(web::WebContext) std::byte context_storage[sizeof(web::WebContext)];
            auto *ctx = std::construct_at(reinterpret_cast<web::WebContext *>(context_storage), make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions));
            ctx->event_bus = &event_bus;
            web::handle_chat(*ctx, req, res);
            std::destroy_at(ctx);
            ctx = std::construct_at(reinterpret_cast<web::WebContext *>(context_storage), web::WebContext{});

            const auto body = drain_response_stream(res);

            std::destroy_at(ctx);
            CHECK(sessions.empty());
            CHECK(sse_event_names(body) == std::vector<std::string>{"session", "text", "done"});
            CHECK(bus_events == std::vector<std::string>{"chat.session_started", "chat.text", "chat.done"});

            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("chat_handler_streaming_provider_error_removes_active_session_and_publishes_error") {
            StreamingOpenAiServer provider({ProviderHttpResponse{.status = 500, .body = R"({"error":"upstream exploded"})"}});
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-stream-error");
            config.profiles.at("shared").base_url = provider.base_url();
            config.agents["default"].workspace = workspace.string();
            WebChatStoreHarness store_harness;
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;
            web::EventBus event_bus;
            std::vector<std::string> bus_events;
            auto subscription = event_bus.subscribe([&bus_events](const web::BusEvent &event) {
                bus_events.push_back(event.kind);
            });

            httplib::Request req;
            req.body = R"({"message":"hello","agent_key":"default"})";
            httplib::Response res;
            auto ctx = make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions);
            ctx.event_bus = &event_bus;

            web::handle_chat(ctx, req, res);

            REQUIRE(sessions.size() == 1UL);
            const auto body = drain_response_stream(res);

            CHECK(sessions.empty());
            CHECK(sse_event_names(body) == std::vector<std::string>{"session", "error"});
            const auto error_event = find_sse_event_payload(body, "error");
            REQUIRE(error_event.has_value());
            CHECK(error_event->at("error").get<std::string>().contains("upstream exploded"));
            CHECK(bus_events == std::vector<std::string>{"chat.session_started", "chat.error"});

            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("chat_handler_abort_during_pending_approval_cancels_and_removes_active_session") {
            StreamingOpenAiServer provider({
                ProviderHttpResponse{.body = openai_shell_tool_stream()},
                ProviderHttpResponse{.body = openai_text_stream("after abort")},
            });
            Config config = make_config();
            const auto workspace = orangutan::testing::unique_test_root("web-chat-stream-abort");
            config.profiles.at("shared").base_url = provider.base_url();
            config.agents["default"].workspace = workspace.string();
            WebChatStoreHarness store_harness;
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;

            httplib::Request req;
            req.body = R"({"message":"run shell","agent_key":"default"})";
            httplib::Response res;
            const auto ctx = make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions);

            web::handle_chat(ctx, req, res);

            REQUIRE(sessions.size() == 1UL);
            bool abort_sent = false;
            int abort_status = 0;
            const auto body = drain_response_stream(res, [&](std::string_view chunk) {
                if (abort_sent || !chunk.contains("event: approval_request")) {
                    return;
                }
                std::string active_session_id;
                {
                    std::scoped_lock lock(sessions_mutex);
                    REQUIRE(sessions.size() == 1UL);
                    active_session_id = sessions.begin()->first;
                    REQUIRE(sessions.begin()->second->pending_approval != nullptr);
                }

                httplib::Request abort_req;
                abort_req.body = nlohmann::json{{"session_id", active_session_id}}.dump();
                httplib::Response abort_res;
                web::handle_chat_abort(ctx, abort_req, abort_res);
                abort_sent = true;
                abort_status = abort_res.status;
            });

            CHECK(abort_sent);
            CHECK(abort_status == 200);
            CHECK(sessions.empty());
            CHECK(find_sse_event_payload(body, "approval_request").has_value());
            CHECK(find_sse_event_payload(body, "done").has_value());

            std::filesystem::remove_all(workspace);
        };

        TEST_CASE("help_slash_command_streams_help_without_api_key") {
            orangutan::testing::ScopedEnvVar anthropic_api_key("ANTHROPIC_API_KEY", "");
            orangutan::testing::ScopedEnvVar llm_api_key("LLM_API_KEY", "");

            Config config = make_config();
            config.profiles.at("shared").api_key.clear();
            WebChatServerHarness harness(&config);

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"/help","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text").get<std::string>().contains("/automation run <id>"));
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

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"hello","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 500);
            const auto body = nlohmann::json::parse(res->body);
            CHECK(body["error"]["message"].get<std::string>().contains("failed to resolve workspace for agent 'default'"));

            std::filesystem::remove(workspace_file);
        };

        TEST_CASE("chat_endpoint_rejects_read_only_channel_session") {
            WebChatStoreHarness store_harness;
            Config config = make_config();
            const auto session_id =
                store_harness.store().save({Message::user().text("hello")}, make_session_metadata("test", "agent:default|jid:qqbot:c2c:42", "default", "channel", "qqbot:c2c:42"));
            WebChatServerHarness harness(&config, &store_harness.store());

            const auto res =
                harness.client().Post("/api/v1/chat", nlohmann::json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

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
                harness.client().Post("/api/v1/chat", nlohmann::json{{"message", "hello again"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

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

            const auto ctx = make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions);
            web::handle_chat(ctx, req, res);

            CHECK(res.status == 409);
            REQUIRE_FALSE(res.body.empty());
            const auto body = nlohmann::json::parse(res.body);
            CHECK(body.at("error").at("code") == "session_active");
            CHECK(body.at("error").at("message") == "session already active");
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

            const auto ctx = make_web_context(&config, &store_harness.store(), nullptr, sessions_mutex, sessions);
            web::handle_chat(ctx, req, res);

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
                harness.client().Post("/api/v1/chat", nlohmann::json{{"message", std::string("/resume ") + session_id}, {"agent_key", "default"}}.dump(), "application/json");

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

            const auto res = harness.client().Post("/api/v1/chat", nlohmann::json{{"message", "/new"}, {"agent_key", "default"}, {"session_id", existing_session_id}}.dump(),
                                                   "application/json");

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
                harness.client().Post("/api/v1/chat", nlohmann::json{{"message", "/export"}, {"agent_key", "default"}, {"session_id", session_id}}.dump(), "application/json");

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

            const auto res = harness.client().Post("/api/v1/chat/abort", R"({"session_id":"nonexistent"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 404);
        };

        TEST_CASE("abort_endpoint_rejects_missing_session_id") {
            WebChatServerHarness harness;

            const auto res = harness.client().Post("/api/v1/chat/abort", "{}", "application/json");

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
            config.agents["default"].permissions_config = {};
            config.agents["default"].fallback_models = {FallbackModelRef{"coder-test"}};
            config.agents["coder"].workspace = workspace.string();

            MemoryStore memory_store((workspace / "memory.db"));
            std::string session_id = "web-chat-runtime-session";
            bootstrap::AppRuntime app_runtime(workspace / "automation.db");
            auto completion_resume_state = std::make_shared<orangutan::web::WebCompletionResumeState>();
            completion_resume_state->agent_key = "default";
            completion_resume_state->automation_runtime = &app_runtime.automation_runtime();

            auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, &app_runtime.automation_service(),
                                                                 &app_runtime.automation_runtime(), {}, completion_resume_state);

            CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "custom_echo"));
            CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "tool_search"));
            CHECK(runtime.tool_context().runtime_key == "agent:default|web:local");
            CHECK(runtime.tool_context().scope_key == "agent:default|web");
            CHECK(runtime.tool_context().runtime_origin == base::origin::web);
            CHECK(runtime.tool_context().raw_caller_id == "web:local");
            CHECK(runtime.tool_context().current_session_id == &session_id);
            CHECK(runtime.tool_context().approval_callback != nullptr);
            CHECK(runtime.tool_context().automation_service == &app_runtime.automation_service());
            CHECK(runtime.tool_context().automation_runtime == &app_runtime.automation_runtime());
            REQUIRE(runtime.tool_context().background_completion_runtime != nullptr);
            CHECK(runtime.tool_context().background_completion_runtime->supports_completion_routing());
            CHECK(runtime.tool_context().background_completion_runtime->supports_resume_callback());
            REQUIRE(runtime.agent != nullptr);

            const auto shell_result = runtime.tools().execute(ToolUse("web-shell", "shell", {{"command", "echo hello"}}));
            CHECK(shell_result.is_error);
            CHECK((shell_result.content.contains("Requires approval") || shell_result.content.contains("Rejected by user")));
        };

        TEST_CASE("automation_slash_command_uses_runtime_tool_output") {
            Config config = make_config();
            bootstrap::AppRuntime app_runtime(orangutan::testing::unique_test_db_path("web-chat-tasks", "automation.db"));
            WebChatServerHarness harness(&config, nullptr, &app_runtime.automation_runtime());

            const auto res = harness.client().Post("/api/v1/chat", R"({"message":"/automation","agent_key":"default"})", "application/json");

            REQUIRE(static_cast<bool>(res));
            CHECK(res->status == 200);
            const auto text_event = find_sse_event_payload(res->body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text") == "## Automation\n- No automations configured.");
        };

        TEST_CASE("runtime_slash_command_does_not_leave_active_session_entry") {
            Config config = make_config();
            bootstrap::AppRuntime app_runtime(orangutan::testing::unique_test_db_path("web-chat-runtime-command", "automation.db"));
            std::mutex sessions_mutex;
            std::unordered_map<std::string, std::unique_ptr<web::WebSessionState>> sessions;

            httplib::Request req;
            req.body = R"({"message":"/automation","agent_key":"default"})";
            httplib::Response res;
            auto ctx = make_web_context(&config, nullptr, nullptr, sessions_mutex, sessions);
            ctx.automation_runtime = &app_runtime.automation_runtime();

            web::handle_chat(ctx, req, res);

            REQUIRE(static_cast<bool>(res.content_provider_));
            CHECK(sessions.empty());
            const auto body = drain_response_stream(res);
            const auto text_event = find_sse_event_payload(body, "text");
            REQUIRE(text_event.has_value());
            CHECK(text_event->at("text") == "## Automation\n- No automations configured.");
            CHECK(sessions.empty());
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

            auto runtime = web::detail::build_web_runtime_bundle(config, "default", &memory_store, &session_id, nullptr, nullptr);

            CHECK(runtime.skills_prompt.contains("web-chat-runtime-skill"));
            REQUIRE(runtime.hook_manager != nullptr);
            CHECK(runtime.hook_manager->hook_count(hook_event::before_tool_call) == 1);
        };

        TEST_CASE("completion_resume_after_session_shutdown_falls_back_to_inbox_notes") {
            const auto automation_db_path = orangutan::testing::unique_test_db_path("web-chat-automation", "automation.db");
            bootstrap::AppRuntime app_runtime(automation_db_path);
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
            resume_state->automation_runtime = &app_runtime.automation_runtime();
            {
                std::scoped_lock lock(resume_state->mutex);
                resume_state->agent = nullptr;
            }

            ToolRuntimeContext tool_context{
                .runtime_key = "agent:default|web:local",
                .agent_key = "default",
                .scope_key = "agent:default|web",
                .automation_service = &app_runtime.automation_service(),
                .automation_runtime = &app_runtime.automation_runtime(),
                .background_completion_runtime = make_background_completion_runtime_bindings(
                    [&app_runtime](const automation::DeliveryRecord &delivery) {
                        static_cast<void>(app_runtime.automation_service().record_delivery(delivery));
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
            const auto deliveries = app_runtime.automation_service().list_deliveries(automation::DeliveryQuery{.agent_key = "default"});
            REQUIRE(deliveries.size() == 2UL);

            const auto *completion_item = find_delivery_by_body_type(deliveries, "background_process_completion");
            const auto *failure_item = find_delivery_by_body_type(deliveries, "background_process_completion_resume_failure");
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
            const auto ctx = make_web_context(nullptr, nullptr, nullptr, sessions_mutex, sessions);
            web::handle_chat_approval(ctx, req, res);

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
            const auto ctx = make_web_context(nullptr, nullptr, nullptr, sessions_mutex, sessions);
            web::handle_chat_abort(ctx, req, res);

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
