#include "web/errors.hpp"
#include "web/event-bus.hpp"
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

    void handle_get_config(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        send_json(res, config_to_json(*ctx.config));
    }

    void handle_put_config(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        const auto input = parse_body(req, res);
        if (!input.has_value()) {
            return;
        }

        *ctx.config = config_detail::parse_json(*input, {});

        const auto config_path = !ctx.config_save_path.empty() ? ctx.config_save_path : config::default_orangutan_config_path();
        if (config_path.empty()) {
            send_error(res, 500, "save_path_unresolved", "unable to resolve config save path");
            return;
        }
        ctx.config->save_to(config_path);
        send_json(res, {{"status", "saved"}, {"path", config_path.string()}});
    }

    void handle_list_tools(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        if (ctx.tool_registry == nullptr) {
            send_error(res, 503, "tools_unavailable", "tool registry not available");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &definition : ctx.tool_registry->definitions()) {
            arr.push_back({
                {"name", definition.name},
                {"description", definition.description},
                {"source", "builtin"},
            });
        }
        send_json(res, {{"items", std::move(arr)}});
    }

    void handle_list_agents(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &[key, agent] : bootstrap::detail::build_effective_agents(*ctx.config)) {
            arr.push_back({
                {"key", key},
                {"profile", agent.profile},
                {"model", agent.model},
                {"workspace", agent.workspace},
                {"edit_mode", agent.edit_mode},
                {"team_agents", agent.team_agents},
            });
        }
        send_json(res, {{"items", std::move(arr)}});
    }

    void handle_agent_graph(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        const auto effective = bootstrap::detail::build_effective_agents(*ctx.config);

        // Snapshot currently-active sessions so the frontend can draw "live" halos
        // around agents that have a runtime attached.
        std::unordered_map<std::string, std::size_t> live_counts;
        if (ctx.sessions != nullptr && ctx.sessions_mutex != nullptr) {
            std::scoped_lock lock(*ctx.sessions_mutex);
            for (const auto &[session_id, state] : *ctx.sessions) {
                if (state == nullptr || state->runtime == nullptr) {
                    continue;
                }
                ++live_counts[state->completion_resume_state != nullptr ? state->completion_resume_state->agent_key : std::string{}];
            }
        }

        auto nodes = nlohmann::json::array();
        auto edges = nlohmann::json::array();
        for (const auto &[key, agent] : effective) {
            auto live_it = live_counts.find(key);
            nodes.push_back({
                {"id", key},
                {"model", agent.model},
                {"profile", agent.profile},
                {"workspace", agent.workspace},
                {"edit_mode", agent.edit_mode},
                {"leader_mode", agent.leader_mode},
                {"team_size", agent.team_agents.size()},
                {"live_sessions", live_it == live_counts.end() ? 0 : live_it->second},
            });
            for (const auto &peer : agent.team_agents) {
                edges.push_back({
                    {"source", key},
                    {"target", peer},
                    {"kind", "team"},
                });
            }
        }

        send_json(res, {
                           {"nodes", std::move(nodes)},
                           {"edges", std::move(edges)},
                           {"generated_at", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
                       });
    }

    void handle_list_skills(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        if (ctx.skill_loader == nullptr) {
            send_error(res, 503, "skills_unavailable", "skill loader not available");
            return;
        }

        auto arr = nlohmann::json::array();
        const auto catalog = ctx.skill_loader->list(skills::skill_list_query{.include_inactive = true});
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

        send_json(res, {
                           {"schema_version", 2},
                           {"items", std::move(arr)},
                       });
    }

    void handle_system_status(const WebContext &ctx, const httplib::Request & /*req*/, httplib::Response &res) {
        const auto now = std::chrono::steady_clock::now();
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - ctx.start_time).count();

        std::size_t active_sessions = 0;
        if (ctx.sessions != nullptr && ctx.sessions_mutex != nullptr) {
            std::scoped_lock lock(*ctx.sessions_mutex);
            active_sessions = ctx.sessions->size();
        }

        nlohmann::json body = {
            {"uptime_seconds", uptime},
            {"active_web_sessions", active_sessions},
            {"provider_health", nlohmann::json::object()},
            {"event_bus",
             {
                 {"latest_sequence", ctx.event_bus != nullptr ? ctx.event_bus->current_sequence() : 0},
             }},
        };
        if (ctx.automation_service == nullptr) {
            body["automation"] = nullptr;
        } else {
            body["automation"] = {
                {"automation_count", ctx.automation_service->list().size()},
            };
        }
        send_json(res, body);
    }

    void handle_server_info(const WebContext & /*ctx*/, const httplib::Request & /*req*/, httplib::Response &res) {
        send_json(res, {
                           {"name", "orangutan"},
                           {"api_version", "v1"},
                           {"capabilities", nlohmann::json::array({"sse", "events", "graph", "pagination", "approvals", "automation"})},
                       });
    }

} // namespace orangutan::web
