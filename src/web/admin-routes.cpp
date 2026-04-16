#include "web/web-route-internal.hpp"

#include "bootstrap/bootstrap.hpp"
#include "config/config-detail.hpp"
#include "config/secret-protection.hpp"
#include "skills/skill-loader.hpp"
#include "tools/automation/automation-tool-support.hpp"
#include "tools/registry/tool-registry.hpp"

#include <magic_enum/magic_enum.hpp>

namespace orangutan::web {

    namespace bootstrap = orangutan::bootstrap;

    namespace {

        nlohmann::json permission_config_to_json(const PermissionConfig &permissions_config) {
            return {
                {"default_mode", std::string{magic_enum::enum_name(permissions_config.default_mode)}},
                {"allow", permissions_config.allow},
                {"deny", permissions_config.deny},
                {"ask", permissions_config.ask},
            };
        }

        nlohmann::json fallback_models_to_json(const std::vector<config::FallbackModelRef> &fallback_models) {
            auto array = nlohmann::json::array();
            for (const auto &fallback : fallback_models) {
                if (fallback.profile.empty()) {
                    array.push_back(fallback.model);
                } else {
                    array.push_back({
                        {"profile", fallback.profile},
                        {"model", fallback.model},
                    });
                }
            }
            return array;
        }

        nlohmann::json model_config_to_json(const config::ModelConfig &model_cfg) {
            nlohmann::json json = {
                {"provider", model_cfg.provider},
                {"protocol", model_cfg.protocol},
            };
            if (model_cfg.max_tokens.has_value()) {
                json["max_tokens"] = *model_cfg.max_tokens;
            }
            if (model_cfg.context_window.has_value()) {
                json["context_window"] = *model_cfg.context_window;
            }
            if (!model_cfg.thinking.empty()) {
                json["thinking"] = model_cfg.thinking;
            }
            if (model_cfg.cost.has_value()) {
                json["cost"] = {
                    {"input", model_cfg.cost->input},
                    {"output", model_cfg.cost->output},
                };
            }
            return json;
        }

        nlohmann::json profile_config_to_json(const config::ProfileConfig &profile_cfg) {
            nlohmann::json json = {
                {"base_url", profile_cfg.base_url},
                {"api_key", profile_cfg.api_key},
                {"headers", profile_cfg.headers},
            };
            nlohmann::json models = nlohmann::json::object();
            for (const auto &[model_name, model_cfg] : profile_cfg.models) {
                models[model_name] = model_config_to_json(model_cfg);
            }
            json["models"] = std::move(models);
            return json;
        }

        nlohmann::json agent_config_to_json(const config::AgentConfig &agent_cfg) {
            nlohmann::json json = {
                {"model", agent_cfg.model},
                {"workspace", agent_cfg.workspace},
                {"edit_mode", agent_cfg.edit_mode},
                {"team_agents", agent_cfg.team_agents},
                {"fallback_models", fallback_models_to_json(agent_cfg.fallback_models)},
                {"thinking_budget", agent_cfg.thinking_budget},
            };
            if (!agent_cfg.profile.empty()) {
                json["profile"] = agent_cfg.profile;
            }
            json["permissions"] = permission_config_to_json(agent_cfg.permissions_config);
            return json;
        }

        nlohmann::json config_to_json(const config::Config &cfg) {
            nlohmann::json body = {
                {"agent",
                 {
                     {"profile", cfg.profile},
                     {"model", cfg.model},
                     {"temperature", cfg.temperature},
                     {"max_iterations", cfg.max_iterations},
                     {"max_tokens", cfg.max_tokens},
                     {"workspace", cfg.workspace},
                     {"edit_mode", cfg.edit_mode},
                     {"fallback_models", fallback_models_to_json(cfg.fallback_models)},
                     {"thinking_budget", cfg.thinking_budget},
                 }},
                {"session", {{"auto_save", cfg.auto_save}}},
                {"tools", {{"edit_mode", cfg.edit_mode}}},
                {"permissions", permission_config_to_json(cfg.permissions_config)},
                {"memory",
                 {
                     {"mirror_enabled", cfg.memory.mirror_enabled},
                     {"mirror_file", cfg.memory.mirror_file},
                     {"journal_dir", cfg.memory.journal_dir},
                 }},
            };

            nlohmann::json profiles = nlohmann::json::object();
            for (const auto &[profile_name, profile_cfg] : cfg.profiles) {
                profiles[profile_name] = profile_config_to_json(profile_cfg);
            }
            body["profiles"] = std::move(profiles);

            nlohmann::json agents = nlohmann::json::object();
            for (const auto &[agent_key, agent_cfg] : cfg.agents) {
                agents[agent_key] = agent_config_to_json(agent_cfg);
            }
            body["agents"] = std::move(agents);
            return body;
        }

    } // namespace

    void handle_get_config(const httplib::Request & /*req*/, httplib::Response &res, config::Config *config) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        const nlohmann::json body = config_to_json(*config);
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

        *config = config_detail::parse_json(input, {});

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
                {"profile", agent.profile},
                {"model", agent.model},
                {"workspace", agent.workspace},
                {"edit_mode", agent.edit_mode},
                {"team_agents", agent.team_agents},
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
        const auto catalog = loader->list(skills::skill_list_query{.include_inactive = true});
        for (const auto &skill : catalog.skills) {
            arr.push_back({
                {"id", skill.id},
                {"name", skill.name},
                {"description", skill.description},
                {"tools", skill.tools},
                {"source", std::string{magic_enum::enum_name(skill.source)}},
                {"scope", std::string{magic_enum::enum_name(skill.scope)}},
                {"active", skill.active},
                {"diagnostic_count", skill.diagnostics.size()},
                {"source_path", skill.source_path},
            });
        }

        nlohmann::json body = {
            {"schema_version", 2},
            {"skills", std::move(arr)},
        };
        res.set_content(body.dump(), "application/json");
    }


    void handle_system_status(const httplib::Request & /*req*/, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                              const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions, automation::AutomationService *automation_service) {
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
        if (automation_service == nullptr) {
            body["automation"] = nullptr;
        } else {
            const auto automations = automation_service->list();
            body["automation"] = {
                {"automation_count", automations.size()},
            };
        }
        res.set_content(body.dump(), "application/json");
    }

} // namespace orangutan::web
