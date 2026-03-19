#include "features/web/web-routes.hpp"
#include "features/web/web-types.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/config/config.hpp"
#include "core/tools/tool.hpp"
#include "features/skills/skill-loader.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::web {

void handle_list_sessions(const httplib::Request & /*req*/, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto sessions = store->list_sessions();
    auto arr = nlohmann::json::array();
    for (const auto &s : sessions) {
        arr.push_back({
            {"id", s.id},
            {"created_at", s.created_at},
            {"model", s.model},
            {"message_count", s.message_count},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_get_session(const httplib::Request &req, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto session_id = std::string(req.matches[1]);
    std::vector<Message> messages;
    try {
        messages = store->load(session_id);
    } catch (const std::runtime_error &) {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &msg : messages) {
        arr.push_back(message_to_json(msg));
    }
    nlohmann::json body = {
        {"id", session_id},
        {"messages", arr},
    };
    res.set_content(body.dump(), "application/json");
}

void handle_delete_session(const httplib::Request &req, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto session_id = std::string(req.matches[1]);
    store->remove(session_id);
    res.set_content(R"({"status":"deleted"})", "application/json");
}

void handle_get_config(const httplib::Request & /*req*/, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    nlohmann::json body = {
        {"provider", config->provider},
        {"model", config->model},
        {"base_url", config->base_url},
        {"temperature", config->temperature},
        {"max_iterations", config->max_iterations},
        {"max_tokens", config->max_tokens},
        {"workspace", config->workspace},
        {"edit_mode", config->edit_mode},
        {"system_prompt", config->system_prompt},
        {"auto_save", config->auto_save},
        {"allowed_tools", config->allowed_tools},
        {"denied_tools", config->denied_tools},
        {"fallback_models", config->fallback_models},
        {"memory",
         {
             {"mirror_enabled", config->memory.mirror_enabled},
             {"mirror_file", config->memory.mirror_file},
             {"journal_dir", config->memory.journal_dir},
         }},
    };
    // Intentionally omit api_key for security
    res.set_content(body.dump(), "application/json");
}

void handle_put_config(const httplib::Request &req, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    nlohmann::json input;
    try {
        input = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error &e) {
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON"})", "application/json");
        return;
    }

    // Update mutable fields if present
    if (input.contains("model")) {
        config->model = input["model"].get<std::string>();
    }
    if (input.contains("provider")) {
        config->provider = input["provider"].get<std::string>();
    }
    if (input.contains("base_url")) {
        config->base_url = input["base_url"].get<std::string>();
    }
    if (input.contains("temperature")) {
        config->temperature = input["temperature"].get<double>();
    }
    if (input.contains("max_iterations")) {
        config->max_iterations = input["max_iterations"].get<int>();
    }
    if (input.contains("max_tokens")) {
        config->max_tokens = input["max_tokens"].get<int>();
    }
    if (input.contains("workspace")) {
        config->workspace = input["workspace"].get<std::string>();
    }
    if (input.contains("edit_mode")) {
        config->edit_mode = input["edit_mode"].get<std::string>();
    }
    if (input.contains("system_prompt")) {
        config->system_prompt = input["system_prompt"].get<std::string>();
    }
    if (input.contains("auto_save")) {
        config->auto_save = input["auto_save"].get<bool>();
    }

    auto config_path = expand_home_path("~/.orangutan/config.toml");
    config->save_to(config_path);

    res.set_content(R"({"status":"saved"})", "application/json");
}

void handle_list_tools(const httplib::Request & /*req*/, httplib::Response &res, ToolRegistry *registry) {
    if (registry == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"tool registry not available"})", "application/json");
        return;
    }
    auto defs = registry->definitions();
    auto arr = nlohmann::json::array();
    for (const auto &d : defs) {
        arr.push_back({
            {"name", d.name},
            {"description", d.description},
            {"source", "builtin"},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_agents(const httplib::Request & /*req*/, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &[key, agent] : config->agents) {
        arr.push_back({
            {"key", key},
            {"provider", agent.provider},
            {"model", agent.model},
            {"base_url", agent.base_url},
            {"system_prompt", agent.system_prompt},
            {"workspace", agent.workspace},
            {"edit_mode", agent.edit_mode},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_skills(const httplib::Request & /*req*/, httplib::Response &res, SkillLoader *loader) {
    if (loader == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"skill loader not available"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &s : loader->active_skills()) {
        arr.push_back({
            {"name", s.name},
            {"description", s.description},
            {"tools", s.tools},
            {"source_path", s.source_path},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_system_status(const httplib::Request & /*req*/, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                          const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    size_t active_sessions = 0;
    {
        std::lock_guard lock(sessions_mutex);
        active_sessions = sessions.size();
    }

    nlohmann::json body = {
        {"uptime_seconds", uptime},         {"active_web_sessions", active_sessions}, {"provider_health", nlohmann::json::object()},
        {"cron", nlohmann::json::object()}, {"heartbeat", nlohmann::json::object()},
    };
    res.set_content(body.dump(), "application/json");
}

} // namespace orangutan::web
