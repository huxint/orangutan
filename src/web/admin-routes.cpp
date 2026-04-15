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

        void set_json_error(httplib::Response &res, int status, std::string_view message) {
            res.status = status;
            res.set_content(nlohmann::json{{"error", message}}.dump(), "application/json");
        }

        [[nodiscard]]
        std::expected<nlohmann::json, std::string> parse_request_json(const httplib::Request &req) {
            try {
                return nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return std::unexpected("invalid JSON");
            }
        }

        [[nodiscard]]
        std::expected<std::optional<bool>, std::string> parse_optional_bool_param(const httplib::Request &req, std::string_view key) {
            if (!req.has_param(std::string(key))) {
                return std::optional<bool>{};
            }

            const auto value = req.get_param_value(std::string(key));
            if (value == "true" || value == "1") {
                return std::optional<bool>{true};
            }
            if (value == "false" || value == "0") {
                return std::optional<bool>{false};
            }
            return std::unexpected(std::string(key) + " must be true or false");
        }

        [[nodiscard]]
        std::string resolve_optional_agent_key(const httplib::Request &req, const nlohmann::json *body = nullptr) {
            if (body != nullptr) {
                if (const auto it = body->find("agent_key"); it != body->end() && it->is_string()) {
                    return it->get<std::string>();
                }
            }
            if (req.has_param("agent_key")) {
                return req.get_param_value("agent_key");
            }
            return {};
        }

        [[nodiscard]]
        std::string resolve_agent_key_or_default(const httplib::Request &req, const nlohmann::json *body = nullptr) {
            const auto agent_key = resolve_optional_agent_key(req, body);
            return agent_key.empty() ? std::string{"default"} : agent_key;
        }

        [[nodiscard]]
        nlohmann::json unix_time_to_json(const std::optional<base::i64> &value) {
            if (!value.has_value()) {
                return nullptr;
            }
            return *value;
        }

        [[nodiscard]]
        nlohmann::json automation_to_json(const automation::Automation &automation) {
            return {
                {"id", automation.id},
                {"agent_key", automation.agent_key},
                {"name", automation.name},
                {"prompt", automation.prompt},
                {"notes", automation.notes},
                {"enabled", automation.enabled},
                {"paused", automation.paused},
                {"trigger", automation::trigger_to_json(automation.trigger)},
                {"delivery", automation::delivery_policy_to_json(automation.delivery)},
                {"tags", automation.tags},
                {"last_run_at", unix_time_to_json(automation.last_run_at)},
                {"next_due_at", unix_time_to_json(automation.next_due_at)},
                {"last_status", automation.last_status},
            };
        }

        [[nodiscard]]
        nlohmann::json run_to_json(const automation::RunRecord &run) {
            return {
                {"id", run.id},
                {"automation_id", run.automation_id},
                {"agent_key", run.agent_key},
                {"automation_name", run.automation_name},
                {"started_at", run.started_at},
                {"finished_at", unix_time_to_json(run.finished_at)},
                {"status", run.status},
                {"summary", run.summary},
                {"reply", run.reply},
                {"delivery_status", run.delivery_status},
                {"log_path", run.log_path},
            };
        }

        [[nodiscard]]
        nlohmann::json delivery_to_json(const automation::DeliveryRecord &delivery) {
            return {
                {"id", delivery.id},
                {"run_id", delivery.run_id},
                {"automation_id", delivery.automation_id},
                {"agent_key", delivery.agent_key},
                {"target", delivery.target},
                {"status", delivery.status},
                {"title", delivery.title},
                {"body", delivery.body},
                {"created_at", delivery.created_at},
                {"acked_at", unix_time_to_json(delivery.acked_at)},
            };
        }

        [[nodiscard]]
        std::expected<automation::AutomationQuery, std::string> parse_automation_query(const httplib::Request &req) {
            automation::AutomationQuery query;
            query.agent_key = resolve_optional_agent_key(req);

            const auto enabled = parse_optional_bool_param(req, "enabled");
            if (!enabled.has_value()) {
                return std::unexpected(enabled.error());
            }
            query.enabled = *enabled;

            const auto paused = parse_optional_bool_param(req, "paused");
            if (!paused.has_value()) {
                return std::unexpected(paused.error());
            }
            query.paused = *paused;
            return query;
        }

        [[nodiscard]]
        std::expected<automation::RunQuery, std::string> parse_run_query(const httplib::Request &req) {
            automation::RunQuery query;
            query.agent_key = resolve_optional_agent_key(req);
            if (req.has_param("automation_id")) {
                query.automation_id = req.get_param_value("automation_id");
            }
            return query;
        }

        [[nodiscard]]
        std::expected<automation::DeliveryQuery, std::string> parse_delivery_query(const httplib::Request &req, const nlohmann::json *body = nullptr) {
            automation::DeliveryQuery query;
            query.agent_key = resolve_optional_agent_key(req, body);
            if (req.has_param("automation_id")) {
                query.automation_id = req.get_param_value("automation_id");
            } else if (body != nullptr) {
                if (const auto it = body->find("automation_id"); it != body->end() && it->is_string()) {
                    query.automation_id = it->get<std::string>();
                }
            }
            if (req.has_param("run_id")) {
                query.run_id = req.get_param_value("run_id");
            } else if (body != nullptr) {
                if (const auto it = body->find("run_id"); it != body->end() && it->is_string()) {
                    query.run_id = it->get<std::string>();
                }
            }
            if (req.has_param("target")) {
                query.target = req.get_param_value("target");
            } else if (body != nullptr) {
                if (const auto it = body->find("target"); it != body->end() && it->is_string()) {
                    query.target = it->get<std::string>();
                }
            }

            const auto only_unacked = parse_optional_bool_param(req, "only_unacked");
            if (!only_unacked.has_value()) {
                return std::unexpected(only_unacked.error());
            }
            query.only_unacked = only_unacked->value_or(false);
            return query;
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

    void handle_list_automations(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_automation_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &automation : automation_service->list(*query)) {
            arr.push_back(automation_to_json(automation));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_create_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto body = parse_request_json(req);
        if (!body.has_value()) {
            set_json_error(res, 400, body.error());
            return;
        }

        const auto automation = builtin::detail::parse_create_request(*body, resolve_agent_key_or_default(req, &*body));
        if (!automation.has_value()) {
            set_json_error(res, 400, automation.error());
            return;
        }

        try {
            const auto id = automation_service->save(*automation);
            const auto stored = automation_service->find(automation->agent_key, id);
            res.status = 201;
            res.set_content((stored.has_value() ? automation_to_json(*stored) : nlohmann::json{{"id", id}}).dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
    }

    void handle_get_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto automation = automation_service->find(resolve_agent_key_or_default(req), std::string(req.matches[1]));
        if (!automation.has_value()) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(automation_to_json(*automation).dump(), "application/json");
    }

    void handle_patch_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto body = parse_request_json(req);
        if (!body.has_value()) {
            set_json_error(res, 400, body.error());
            return;
        }

        const auto agent_key = resolve_agent_key_or_default(req, &*body);
        const auto existing = automation_service->find(agent_key, std::string(req.matches[1]));
        if (!existing.has_value()) {
            set_json_error(res, 404, "automation not found");
            return;
        }

        const auto updated = builtin::detail::apply_update_request(*body, *existing, agent_key);
        if (!updated.has_value()) {
            set_json_error(res, 400, updated.error());
            return;
        }

        try {
            const auto id = automation_service->save(*updated);
            const auto stored = automation_service->find(agent_key, id);
            res.set_content((stored.has_value() ? automation_to_json(*stored) : nlohmann::json{{"id", id}}).dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
    }

    void handle_delete_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->remove(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"deleted"})", "application/json");
    }

    void handle_run_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        try {
            const auto run_id = automation_service->run_now(resolve_agent_key_or_default(req), std::string(req.matches[1]));
            res.set_content(nlohmann::json{{"run_id", run_id}}.dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, std::string_view(error.what()) == "automation not found" ? 404 : 400, error.what());
        }
    }

    void handle_pause_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->pause(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"paused"})", "application/json");
    }

    void handle_resume_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->resume(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"resumed"})", "application/json");
    }

    void handle_list_automation_runs(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_run_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &run : automation_service->list_runs(*query)) {
            arr.push_back(run_to_json(run));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_automation_deliveries(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_delivery_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &delivery : automation_service->list_deliveries(*query)) {
            arr.push_back(delivery_to_json(delivery));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_ack_automation_delivery(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        std::optional<nlohmann::json> body;
        if (!req.body.empty()) {
            const auto parsed = parse_request_json(req);
            if (!parsed.has_value()) {
                set_json_error(res, 400, parsed.error());
                return;
            }
            body = *parsed;
        }

        const auto agent_key = resolve_optional_agent_key(req, body ? &*body : nullptr);
        if (agent_key.empty()) {
            set_json_error(res, 400, "agent_key is required");
            return;
        }

        if (!automation_service->ack_delivery(agent_key, std::string(req.matches[1]))) {
            set_json_error(res, 404, "delivery not found");
            return;
        }
        res.set_content(R"({"status":"acknowledged"})", "application/json");
    }

    void handle_clear_automation_deliveries(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        std::optional<nlohmann::json> body;
        if (!req.body.empty()) {
            const auto parsed = parse_request_json(req);
            if (!parsed.has_value()) {
                set_json_error(res, 400, parsed.error());
                return;
            }
            body = *parsed;
        }

        const auto query = parse_delivery_query(req, body ? &*body : nullptr);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }
        if (query->agent_key.empty()) {
            set_json_error(res, 400, "agent_key is required");
            return;
        }

        try {
            automation_service->clear_deliveries(*query);
            res.set_content(R"({"status":"cleared"})", "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
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
