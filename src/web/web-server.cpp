#include "web/web-server.hpp"
#include "web/web-routes.hpp"
#include <spdlog/spdlog.h>

namespace orangutan::web {

    WebServer::WebServer() = default;

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
        session_store_ = store;
    }
    void WebServer::set_memory_store(memory::MemoryStore *store) {
        memory_store_ = store;
    }
    void WebServer::set_config(config::Config *config) {
        config_ = config;
    }
    void WebServer::set_config_save_path(const std::filesystem::path &path) {
        config_save_path_ = path;
    }
    void WebServer::set_tool_registry(tools::ToolRegistry *registry) {
        tool_registry_ = registry;
    }
    void WebServer::set_skill_loader(skills::SkillLoader *loader) {
        skill_loader_ = loader;
    }
    void WebServer::set_automation_runtime(automation::Runtime *runtime) {
        automation_runtime_ = runtime;
    }

    void WebServer::setup_routes() {
        server_.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (!static_dir_.empty()) {
                res.set_header("Access-Control-Allow-Origin", req.get_header_value("Origin"));
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type");
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

        server_.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
            res.set_content(R"({"status":"ok"})", "application/json");
        });

        server_.Get(R"(/api/sessions/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_get_session(req, res, session_store_);
        });

        server_.Delete(R"(/api/sessions/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_delete_session(req, res, session_store_);
        });

        server_.Get("/api/sessions", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_sessions(req, res, session_store_);
        });

        server_.Get("/api/config", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_get_config(req, res, config_);
        });

        server_.Put("/api/config", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_put_config(req, res, config_, config_save_path_.empty() ? nullptr : &config_save_path_);
        });

        server_.Get("/api/tools", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_tools(req, res, tool_registry_);
        });

        server_.Get("/api/agents", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_agents(req, res, config_);
        });

        server_.Get(R"(/api/agents/([^/]+)/sessions)", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_agent_sessions(req, res, config_, session_store_);
        });

        server_.Get(R"(/api/agents/([^/]+)/sessions/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_get_agent_session(req, res, config_, session_store_);
        });

        server_.Delete(R"(/api/agents/([^/]+)/sessions/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_delete_agent_session(req, res, config_, session_store_);
        });

        server_.Get("/api/skills", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_skills(req, res, skill_loader_);
        });

        server_.Get("/api/tasks", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_tasks(req, res, automation_runtime_);
        });

        server_.Get("/api/heartbeats", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_heartbeats(req, res, automation_runtime_);
        });

        server_.Get("/api/inbox", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_list_inbox(req, res, automation_runtime_);
        });

        server_.Post("/api/inbox/ack", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_ack_inbox(req, res, automation_runtime_);
        });

        server_.Delete("/api/inbox", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_clear_inbox(req, res, automation_runtime_);
        });

        server_.Get("/api/system/status", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_system_status(req, res, start_time_, sessions_mutex_, sessions_, automation_runtime_);
        });

        server_.Get("/api/system", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_system_status(req, res, start_time_, sessions_mutex_, sessions_, automation_runtime_);
        });

        server_.Post("/api/chat", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_chat(req, res, config_, session_store_, memory_store_, tool_registry_, automation_runtime_, sessions_mutex_, sessions_);
        });

        server_.Post("/api/chat/approval", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_chat_approval(req, res, sessions_mutex_, sessions_);
        });

        server_.Post("/api/chat/abort", [this](const httplib::Request &req, httplib::Response &res) {
            web::handle_chat_abort(req, res, sessions_mutex_, sessions_);
        });

        if (!static_dir_.empty()) {
            server_.set_mount_point("/", static_dir_.string());
        }
    }

} // namespace orangutan::web
