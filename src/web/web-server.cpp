#include "web/web-server.hpp"
#include "web/web-routes.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::web {

    namespace {

        /// Bind a 3-arg v1 handler to httplib's (req, res) callback using the server's
        /// WebContext. Keeps route registration readable instead of a wall of lambdas.
        template <auto Handler>
        auto bind_handler(const WebContext &ctx) {
            return [&ctx](const httplib::Request &req, httplib::Response &res) {
                Handler(ctx, req, res);
            };
        }

    } // namespace

    WebServer::WebServer() {
        context_.sessions = &sessions_;
        context_.sessions_mutex = &sessions_mutex_;
        context_.event_bus = &event_bus_;
        context_.start_time = std::chrono::steady_clock::now();
    }

    WebServer::~WebServer() {
        stop();
    }

    void WebServer::start(const std::string &host, int port) {
        setup_routes();
        port_ = 0;
        running_ = false;
        {
            std::scoped_lock lock(startup_mutex_);
            startup_complete_ = false;
        }

        server_thread_ = std::jthread([this, host, port] {
            int bound_port = 0;
            if (port == 0) {
                bound_port = server_.bind_to_any_port(host);
                if (bound_port <= 0) {
                    std::scoped_lock lock(startup_mutex_);
                    startup_complete_ = true;
                    startup_cv_.notify_all();
                    return;
                }
            } else {
                if (!server_.bind_to_port(host, port)) {
                    std::scoped_lock lock(startup_mutex_);
                    startup_complete_ = true;
                    startup_cv_.notify_all();
                    return;
                }
                bound_port = port;
            }

            {
                std::scoped_lock lock(startup_mutex_);
                port_ = bound_port;
                startup_complete_ = true;
            }
            startup_cv_.notify_all();

            spdlog::info("web server listening on {}:{}", host, port_);
            server_.listen_after_bind();
            running_ = false;
        });

        std::unique_lock lock(startup_mutex_);
        constexpr auto STARTUP_TIMEOUT = std::chrono::seconds(5);
        if (!startup_cv_.wait_for(lock, STARTUP_TIMEOUT, [this] {
                return startup_complete_;
            })) {
            lock.unlock();
            server_.stop();
            server_thread_.join();
            throw std::runtime_error("web server startup timed out on " + host + ":" + std::to_string(port));
        }

        if (port_ <= 0) {
            lock.unlock();
            server_thread_.join();
            throw std::runtime_error(port == 0 ? "failed to bind web server to any port on " + host : "failed to bind web server to " + host + ":" + std::to_string(port));
        }

        lock.unlock();
        server_.wait_until_ready();
        running_ = server_.is_running();

        if (!running_) {
            server_.stop();
            server_thread_.join();
            throw std::runtime_error("web server failed to start listening on " + host + ":" + std::to_string(port_));
        }
    }

    void WebServer::stop() {
        if (running_) {
            server_.stop();
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            running_ = false;
        }
    }

    bool WebServer::is_running() const {
        return running_;
    }

    int WebServer::port() const {
        return port_;
    }

    void WebServer::set_static_dir(const std::filesystem::path &path) {
        static_dir_ = path;
    }

    void WebServer::set_session_store(storage::SessionStore *store) {
        context_.session_store = store;
    }
    void WebServer::set_memory_store(memory::MemoryStore *store) {
        context_.memory_store = store;
    }
    void WebServer::set_config(config::Config *config) {
        context_.config = config;
    }
    void WebServer::set_config_save_path(const std::filesystem::path &path) {
        context_.config_save_path = path;
    }
    void WebServer::set_tool_registry(tools::ToolRegistry *registry) {
        context_.tool_registry = registry;
    }
    void WebServer::set_skill_loader(skills::SkillLoader *loader) {
        context_.skill_loader = loader;
    }
    void WebServer::set_automation_service(automation::AutomationService *service) {
        context_.automation_service = service;
    }
    void WebServer::set_automation_runtime(automation::AutomationRuntime *runtime) {
        context_.automation_runtime = runtime;
    }

    WebServerBuilder WebServer::configure(WebServer &server) {
        return WebServerBuilder(server);
    }

    void WebServer::setup_routes() {
        const auto &ctx = context_;

        server_.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (!static_dir_.empty()) {
                res.set_header("Access-Control-Allow-Origin", req.get_header_value("Origin"));
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, Last-Event-ID");
            }
            if (req.method == "OPTIONS") {
                res.status = 204;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server_.set_post_routing_handler([](const httplib::Request &req, httplib::Response &res) {
            if (req.path.starts_with("/api/")) {
                res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
                res.set_header("Pragma", "no-cache");
                res.set_header("Expires", "0");
            }
        });

        // Unversioned health endpoint (operators don't want to track API versions).
        server_.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
            res.set_content(R"({"status":"ok"})", "application/json");
        });

        // Server metadata: name, version, capabilities. Frontend hits this on boot to
        // discover what the backend supports (observatory, automation, etc.).
        server_.Get("/api/v1/server", bind_handler<handle_server_info>(ctx));

        // Sessions — flat, not scoped. Retained for operator tooling.
        server_.Get("/api/v1/sessions", bind_handler<handle_list_sessions>(ctx));
        server_.Get(R"(/api/v1/sessions/([^/]+))", bind_handler<handle_get_session>(ctx));
        server_.Delete(R"(/api/v1/sessions/([^/]+))", bind_handler<handle_delete_session>(ctx));

        // Config
        server_.Get("/api/v1/config", bind_handler<handle_get_config>(ctx));
        server_.Put("/api/v1/config", bind_handler<handle_put_config>(ctx));

        // Agents + team graph
        server_.Get("/api/v1/tools", bind_handler<handle_list_tools>(ctx));
        server_.Get("/api/v1/agents", bind_handler<handle_list_agents>(ctx));
        server_.Get("/api/v1/agents/graph", bind_handler<handle_agent_graph>(ctx));

        // Agent-scoped sessions
        server_.Get(R"(/api/v1/agents/([^/]+)/sessions)", bind_handler<handle_list_agent_sessions>(ctx));
        server_.Get(R"(/api/v1/agents/([^/]+)/sessions/([^/]+))", bind_handler<handle_get_agent_session>(ctx));
        server_.Delete(R"(/api/v1/agents/([^/]+)/sessions/([^/]+))", bind_handler<handle_delete_agent_session>(ctx));

        // Skills
        server_.Get("/api/v1/skills", bind_handler<handle_list_skills>(ctx));

        // Automation — CRUD + runs + deliveries
        server_.Get("/api/v1/automation", bind_handler<handle_list_automations>(ctx));
        server_.Post("/api/v1/automation", bind_handler<handle_create_automation>(ctx));
        server_.Get("/api/v1/automation/runs", bind_handler<handle_list_automation_runs>(ctx));
        server_.Get("/api/v1/automation/deliveries", bind_handler<handle_list_automation_deliveries>(ctx));
        server_.Post(R"(/api/v1/automation/deliveries/([^/]+)/ack)", bind_handler<handle_ack_automation_delivery>(ctx));
        server_.Delete("/api/v1/automation/deliveries", bind_handler<handle_clear_automation_deliveries>(ctx));
        server_.Get(R"(/api/v1/automation/([^/]+))", bind_handler<handle_get_automation>(ctx));
        server_.Patch(R"(/api/v1/automation/([^/]+))", bind_handler<handle_patch_automation>(ctx));
        server_.Delete(R"(/api/v1/automation/([^/]+))", bind_handler<handle_delete_automation>(ctx));
        server_.Post(R"(/api/v1/automation/([^/]+)/run)", bind_handler<handle_run_automation>(ctx));
        server_.Post(R"(/api/v1/automation/([^/]+)/pause)", bind_handler<handle_pause_automation>(ctx));
        server_.Post(R"(/api/v1/automation/([^/]+)/resume)", bind_handler<handle_resume_automation>(ctx));

        // System status
        server_.Get("/api/v1/system", bind_handler<handle_system_status>(ctx));
        server_.Get("/api/v1/system/status", bind_handler<handle_system_status>(ctx));

        // Global event stream (observatory). Long-lived SSE connection — every other
        // part of the server publishes onto this bus.
        server_.Get("/api/v1/events", bind_handler<handle_event_stream>(ctx));

        // Chat
        server_.Post("/api/v1/chat", bind_handler<handle_chat>(ctx));
        server_.Post("/api/v1/chat/approval", bind_handler<handle_chat_approval>(ctx));
        server_.Post("/api/v1/chat/abort", bind_handler<handle_chat_abort>(ctx));

        if (!static_dir_.empty()) {
            server_.set_mount_point("/", static_dir_.string());
        }
    }

} // namespace orangutan::web
