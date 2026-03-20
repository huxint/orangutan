#include "features/web/web-server.hpp"
#include "features/web/web-routes.hpp"
#include <spdlog/spdlog.h>

namespace orangutan {

WebServer::WebServer() = default;

WebServer::~WebServer() {
    stop();
}

void WebServer::start(const std::string &host, int port) {
    setup_routes();
    port_ = 0;
    running_ = false;

    server_thread_ = std::thread([this, host, port] {
        if (port == 0) {
            port_ = server_.bind_to_any_port(host);
            if (port_ <= 0) {
                return;
            }
        } else {
            if (!server_.bind_to_port(host, port)) {
                return;
            }
            port_ = port;
        }

        running_ = true;
        spdlog::info("Web server listening on {}:{}", host, port_);
        server_.listen_after_bind();
        running_ = false;
    });

    for (int attempt = 0; attempt < 200 && port_ == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port_ <= 0) {
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        throw std::runtime_error(port == 0 ? "Failed to bind web server to any port on " + host : "Failed to bind web server to " + host + ":" + std::to_string(port));
    }

    for (int attempt = 0; attempt < 200 && !running_; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!running_) {
        server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        throw std::runtime_error("Web server failed to start listening on " + host + ":" + std::to_string(port_));
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

void WebServer::set_static_dir(const std::string &path) {
    static_dir_ = path;
}

void WebServer::set_session_store(SessionStore *store) {
    session_store_ = store;
}
void WebServer::set_memory_store(MemoryStore *store) {
    memory_store_ = store;
}
void WebServer::set_subagent_manager(SubagentManager *manager) {
    subagent_manager_ = manager;
}
void WebServer::set_config(Config *config) {
    config_ = config;
}
void WebServer::set_config_save_path(const std::string &path) {
    config_save_path_ = path;
}
void WebServer::set_tool_registry(ToolRegistry *registry) {
    tool_registry_ = registry;
}
void WebServer::set_skill_loader(SkillLoader *loader) {
    skill_loader_ = loader;
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

    server_.Get("/api/system/status", [this](const httplib::Request &req, httplib::Response &res) {
        web::handle_system_status(req, res, start_time_, sessions_mutex_, sessions_);
    });

    server_.Get("/api/system", [this](const httplib::Request &req, httplib::Response &res) {
        web::handle_system_status(req, res, start_time_, sessions_mutex_, sessions_);
    });

    server_.Post("/api/chat", [this](const httplib::Request &req, httplib::Response &res) {
        web::handle_chat(req, res, config_, session_store_, memory_store_, subagent_manager_, tool_registry_, sessions_mutex_, sessions_);
    });

    server_.Post("/api/chat/approval", [this](const httplib::Request &req, httplib::Response &res) {
        web::handle_chat_approval(req, res, sessions_mutex_, sessions_);
    });

    server_.Post("/api/chat/abort", [this](const httplib::Request &req, httplib::Response &res) {
        web::handle_chat_abort(req, res, sessions_mutex_, sessions_);
    });

    if (!static_dir_.empty()) {
        server_.set_mount_point("/", static_dir_);
    }
}

} // namespace orangutan
