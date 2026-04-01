#include "web/web-route-internal.hpp"

#include "bootstrap/bootstrap.hpp"
#include "config/secret-protection.hpp"
#include "skills/skill-loader.hpp"
#include "tools/registry/tool-registry.hpp"

namespace orangutan::web {

    namespace bootstrap = orangutan::bootstrap;

    void handle_get_config(const httplib::Request & /*req*/, httplib::Response &res, config::Config *config) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        const nlohmann::json body = {
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
        res.set_content(body.dump(), "application/json");
    }

    void handle_put_config(const httplib::Request &req, httplib::Response &res, config::Config *config, const std::filesystem::path *config_save_path) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

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
            config->temperature = input["temperature"].get<base::f64>();
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

        const auto config_path = (config_save_path != nullptr && !config_save_path->empty()) ? *config_save_path : config::default_orangutan_config_path();
        if (config_path.empty()) {
            res.status = 500;
            res.set_content(R"({"error":"unable to resolve config save path"})", "application/json");
            return;
        }
        config->save_to(config_path);

        res.set_content(R"({"status":"saved"})", "application/json");
    }

    void handle_list_tools(const httplib::Request & /*req*/, httplib::Response &res, tools::ToolRegistry *registry) {
        if (registry == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"tool registry not available"})", "application/json");
            return;
        }
        const auto definitions = registry->definitions();
        auto arr = nlohmann::json::array();
        for (const auto &definition : definitions) {
            arr.push_back({
                {"name", definition.name},
                {"description", definition.description},
                {"source", "builtin"},
            });
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_agents(const httplib::Request & /*req*/, httplib::Response &res, config::Config *config) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &[key, agent] : bootstrap::detail::build_effective_agents(*config)) {
            arr.push_back({
                {"key", key},
                {"provider", agent.provider},
                {"model", agent.model},
                {"base_url", agent.base_url},
                {"system_prompt", agent.system_prompt},
                {"workspace", agent.workspace},
                {"edit_mode", agent.edit_mode},
                {"subagents", agent.subagents},
            });
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_skills(const httplib::Request & /*req*/, httplib::Response &res, skills::SkillLoader *loader) {
        if (loader == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"skill loader not available"})", "application/json");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &skill : loader->active_skills()) {
            arr.push_back({
                {"name", skill.name},
                {"description", skill.description},
                {"tools", skill.tools},
                {"source_path", skill.source_path},
            });
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_tasks(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
        if (automation_runtime == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"automation runtime not available"})", "application/json");
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &task : automation_runtime->list_tasks(internal::resolve_agent_key_param(req))) {
            arr.push_back(internal::task_to_json(task));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_heartbeats(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
        if (automation_runtime == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"automation runtime not available"})", "application/json");
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &heartbeat : automation_runtime->list_heartbeats(internal::resolve_agent_key_param(req))) {
            arr.push_back(internal::heartbeat_to_json(heartbeat));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
        if (automation_runtime == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"automation runtime not available"})", "application/json");
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &item : automation_runtime->list_inbox(internal::resolve_agent_key_param(req))) {
            arr.push_back(internal::inbox_item_to_json(item));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_ack_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
        if (automation_runtime == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"automation runtime not available"})", "application/json");
            return;
        }

        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        if (!input.contains("id") || !input["id"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'id' field"})", "application/json");
            return;
        }

        const auto agent_key = input.value("agent_key", std::string("default"));
        if (!automation_runtime->ack_inbox(agent_key, input["id"].get<std::string>())) {
            res.status = 404;
            res.set_content(R"({"error":"inbox item not found"})", "application/json");
            return;
        }
        res.set_content(R"({"status":"acknowledged"})", "application/json");
    }

    void handle_clear_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
        if (automation_runtime == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"automation runtime not available"})", "application/json");
            return;
        }

        automation_runtime->clear_inbox(internal::resolve_agent_key_param(req));
        res.set_content(R"({"status":"cleared"})", "application/json");
    }

    void handle_system_status(const httplib::Request & /*req*/, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                              const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions, automation::Runtime *automation_runtime) {
        const auto now = std::chrono::steady_clock::now();
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        std::size_t active_sessions = 0;
        {
            std::scoped_lock lock(sessions_mutex);
            active_sessions = sessions.size();
        }

        nlohmann::json body = {
            {"uptime_seconds", uptime},
            {"active_web_sessions", active_sessions},
            {"provider_health", nlohmann::json::object()},
        };
        if (automation_runtime == nullptr) {
            body["automation"] = nullptr;
        } else {
            const auto all_tasks = automation_runtime->list_tasks({});
            const auto all_heartbeats = automation_runtime->list_heartbeats({});
            body["automation"] = {
                {"task_count", all_tasks.size()},
                {"heartbeat_count", all_heartbeats.size()},
            };
        }
        res.set_content(body.dump(), "application/json");
    }

} // namespace orangutan::web
