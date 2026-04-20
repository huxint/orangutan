#include "config/config-detail.hpp"
#include "config/secret-fields.hpp"
#include "utils/path.hpp"
#include "utils/transparent-lookup.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::config {

    namespace {

        [[nodiscard]]
        std::runtime_error invalid_config_error(const std::filesystem::path &path, std::string_view message) {
            return std::runtime_error("invalid config file " + path.string() + ": " + std::string(message));
        }

    } // namespace

    std::string expand_env_vars(const std::string &input) {
        std::string result;
        result.reserve(input.size());

        std::size_t pos = 0;
        while (pos < input.size()) {
            auto start = input.find("${", pos);
            if (start == std::string::npos) {
                result.append(input, pos);
                break;
            }

            result.append(input, pos, start - pos);

            auto end = input.find('}', start + 2);
            if (end == std::string::npos) {
                result.append(input, start);
                break;
            }

            auto var_name = input.substr(start + 2, end - start - 2);
            const char *val = std::getenv(var_name.c_str());
            if (val != nullptr) {
                result.append(val);
            } else {
                spdlog::warn("environment variable '{}' is not set, replacing with empty string", var_name);
            }

            pos = end + 1;
        }

        return result;
    }

    namespace detail {

        namespace {

            nlohmann::json serialize_fallback_models(const std::vector<FallbackModelRef> &fallback_models) {
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

        } // namespace

        std::string expand_path_value(std::string input) {
            if (input.size() > 1 && input[0] == '~' && input[1] != '/') {
                spdlog::warn("path '{}' uses unsupported ~user syntax; leaving it unchanged", input);
                return input;
            }

            auto expanded = utils::try_expand_home_path(std::filesystem::path{input});
            if (!expanded.has_value()) {
                spdlog::warn("{}", expanded.error());
                return input;
            }
            return expanded->string();
        }

        void resolve_secret_field(std::string &value, std::string_view field_kind, std::string_view display_field, ConfigPasswordResolver &resolver) {
            if (value.empty()) {
                return;
            }

            if (is_protected_config_secret(value)) {
                value = reveal_config_secret(value, resolver.resolve(), field_kind, display_field);
                return;
            }

            value = expand_env_vars(value);
        }

        Config parse_json(const nlohmann::json &root, const ConfigSecretOptions &secret_options) {
            auto resolved_root = root;
            ConfigPasswordResolver password_resolver(secret_options);
            for (auto &secret : collect_config_secret_json_values(resolved_root)) {
                resolve_secret_field(*secret.value, secret.field_kind, secret.display_field, password_resolver);
            }

            Config cfg;
            cfg = parse_agent_section(resolved_root, std::move(cfg));
            cfg = parse_profiles_section(resolved_root, std::move(cfg));
            cfg = parse_tools_section(resolved_root, std::move(cfg));
            cfg = parse_permissions_section(resolved_root, std::move(cfg));
            cfg = parse_session_section(resolved_root, std::move(cfg));
            cfg = parse_memory_section(resolved_root, std::move(cfg));
            cfg = parse_qq_section(resolved_root, std::move(cfg));

            cfg.profile = expand_env_vars(cfg.profile);
            cfg.model = expand_env_vars(cfg.model);
            cfg.workspace = expand_path_value(expand_env_vars(cfg.workspace));
            for (auto &fallback_model : cfg.fallback_models) {
                fallback_model.profile = expand_env_vars(fallback_model.profile);
                fallback_model.model = expand_env_vars(fallback_model.model);
            }
            for (auto &[_, profile_cfg] : cfg.profiles) {
                expand_profile_config(profile_cfg);
            }
            cfg.memory.mirror_file = expand_path_value(expand_env_vars(cfg.memory.mirror_file));
            cfg.memory.journal_dir = expand_path_value(expand_env_vars(cfg.memory.journal_dir));
            cfg.qq_app_id = expand_env_vars(cfg.qq_app_id);

            cfg = parse_agents_section(resolved_root, std::move(cfg));
            cfg = parse_qq_bots_section(resolved_root, std::move(cfg));
            cfg = parse_security_section(resolved_root, std::move(cfg));
            cfg = parse_skills_section(resolved_root, std::move(cfg));
            cfg = parse_custom_tools_section(resolved_root, std::move(cfg));
            cfg = parse_mcp_section(resolved_root, std::move(cfg));
            cfg = parse_hooks_section(resolved_root, std::move(cfg));
            cfg = parse_heartbeat_section(resolved_root, std::move(cfg));

            for (auto &[key, agent_cfg] : cfg.agents) {
                expand_agent_config(agent_cfg);
            }

            if (cfg.qq_bots.empty() && (!cfg.qq_app_id.empty() || !cfg.qq_client_secret.empty())) {
                cfg.qq_bots.push_back({
                    .name = {},
                    .app_id = cfg.qq_app_id,
                    .client_secret = cfg.qq_client_secret,
                    .agent = "default",
                });
            }

            for (auto &bot : cfg.qq_bots) {
                bot.name = expand_env_vars(bot.name);
                bot.app_id = expand_env_vars(bot.app_id);
                bot.agent = expand_env_vars(bot.agent);

                if (bot.agent.empty()) {
                    bot.agent = "default";
                }
                if (!cfg.agents.contains(bot.agent)) {
                    throw std::runtime_error("QQ bot references unknown agent: " + bot.agent);
                }
            }

            return cfg;
        }

    } // namespace detail

    Config Config::load(const ConfigSecretOptions &secret_options) {
        auto path = default_orangutan_config_path();
        if (path.empty() || !std::filesystem::exists(path)) {
            return {};
        }
        return load_from(path, secret_options);
    }

    Config Config::load_from(const std::filesystem::path &path, const ConfigSecretOptions &secret_options) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                return {};
            }

            nlohmann::json root;
            file >> root;
            return config_detail::parse_json(root, secret_options);
        } catch (const ConfigSecretProtectionError &) {
            throw;
        } catch (const nlohmann::json::parse_error &e) {
            throw invalid_config_error(path, e.what());
        } catch (const nlohmann::json::type_error &e) {
            throw invalid_config_error(path, e.what());
        } catch (const std::runtime_error &e) {
            throw invalid_config_error(path, e.what());
        }
    }

    std::optional<AgentConfig> Config::find_agent(const std::string &key) const {
        if (const auto it = utils::transparent_find(agents, key); it != agents.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void Config::save_to(const std::filesystem::path &path) const {
        nlohmann::json root = nlohmann::json::object();

        nlohmann::json agent = {
            {"model", model},
            {"temperature", temperature},
            {"max_iterations", max_iterations},
            {"max_tokens", max_tokens},
        };
        if (!profile.empty()) {
            agent["profile"] = profile;
        }
        if (thinking_budget != 0) {
            agent["thinking_budget"] = thinking_budget;
        }
        if (!workspace.empty()) {
            agent["workspace"] = workspace;
        }
        if (!fallback_models.empty()) {
            agent["fallback_models"] = detail::serialize_fallback_models(fallback_models);
        }
        root["agent"] = std::move(agent);

        if (!profiles.empty()) {
            nlohmann::json profiles_json = nlohmann::json::object();
            for (const auto &[profile_name, profile_cfg] : profiles) {
                nlohmann::json profile_json = nlohmann::json::object();
                if (!profile_cfg.base_url.empty()) {
                    profile_json["base_url"] = profile_cfg.base_url;
                }
                if (!profile_cfg.api_key.empty()) {
                    profile_json["api_key"] = profile_cfg.api_key;
                }
                if (!profile_cfg.headers.empty()) {
                    profile_json["headers"] = profile_cfg.headers;
                }
                nlohmann::json models_json = nlohmann::json::object();
                for (const auto &[model_name, model_cfg] : profile_cfg.models) {
                    nlohmann::json model_json = nlohmann::json::object();
                    model_json["provider"] = model_cfg.provider;
                    model_json["protocol"] = model_cfg.protocol;
                    if (model_cfg.max_tokens.has_value()) {
                        model_json["max_tokens"] = *model_cfg.max_tokens;
                    }
                    if (model_cfg.context_window.has_value()) {
                        model_json["context_window"] = *model_cfg.context_window;
                    }
                    if (!model_cfg.thinking.empty() && model_cfg.thinking != "none") {
                        model_json["thinking"] = model_cfg.thinking;
                    }
                    if (model_cfg.cost.has_value()) {
                        model_json["cost"] = {
                            {"input", model_cfg.cost->input},
                            {"output", model_cfg.cost->output},
                        };
                    }
                    models_json[model_name] = std::move(model_json);
                }
                if (!models_json.empty()) {
                    profile_json["models"] = std::move(models_json);
                }
                profiles_json[profile_name] = std::move(profile_json);
            }
            root["profiles"] = std::move(profiles_json);
        }

        nlohmann::json tools = {
            {"edit_mode", edit_mode},
        };
        root["tools"] = std::move(tools);

        root["session"] = nlohmann::json{{"auto_save", auto_save}};
        root["memory"] = nlohmann::json{
            {"mirror_enabled", memory.mirror_enabled},
            {"mirror_file", memory.mirror_file},
            {"journal_dir", memory.journal_dir},
        };

        if (!allow.empty() || !deny.empty()) {
            nlohmann::json security = nlohmann::json::object();
            if (!allow.empty()) {
                security["allow"] = allow;
            }
            if (!deny.empty()) {
                security["deny"] = deny;
            }
            root["security"] = std::move(security);
        }

        if (!skill_paths.empty()) {
            root["skills"] = nlohmann::json{{"paths", skill_paths}};
        }

        if (!agents.empty()) {
            nlohmann::json agents_json = nlohmann::json::object();
            for (const auto &[agent_key, agent_cfg] : agents) {
                nlohmann::json agent_json = {
                    {"model", agent_cfg.model},
                };
                if (!agent_cfg.profile.empty()) {
                    agent_json["profile"] = agent_cfg.profile;
                }
                if (!agent_cfg.fallback_models.empty()) {
                    agent_json["fallback_models"] = detail::serialize_fallback_models(agent_cfg.fallback_models);
                }
                if (!agent_cfg.workspace.empty()) {
                    agent_json["workspace"] = agent_cfg.workspace;
                }
                if (!agent_cfg.team_agents.empty()) {
                    agent_json["team_agents"] = agent_cfg.team_agents;
                }
                if (agent_cfg.coordinator_mode) {
                    agent_json["coordinator_mode"] = agent_cfg.coordinator_mode;
                }
                if (agent_cfg.max_concurrent_agents != 4) {
                    agent_json["max_concurrent_agents"] = agent_cfg.max_concurrent_agents;
                }
                if (!agent_cfg.edit_mode.empty() && agent_cfg.edit_mode != "hashline") {
                    agent_json["edit_mode"] = agent_cfg.edit_mode;
                }
                if (agent_cfg.thinking_budget != 0) {
                    agent_json["thinking_budget"] = agent_cfg.thinking_budget;
                }
                if (agent_cfg.permissions_config.default_mode != permission_mode::default_mode || !agent_cfg.permissions_config.allow.empty() ||
                    !agent_cfg.permissions_config.deny.empty() || !agent_cfg.permissions_config.ask.empty()) {
                    nlohmann::json permissions_json = nlohmann::json::object();
                    permissions_json["default_mode"] = std::string{magic_enum::enum_name(agent_cfg.permissions_config.default_mode)};
                    if (!agent_cfg.permissions_config.allow.empty()) {
                        permissions_json["allow"] = agent_cfg.permissions_config.allow;
                    }
                    if (!agent_cfg.permissions_config.deny.empty()) {
                        permissions_json["deny"] = agent_cfg.permissions_config.deny;
                    }
                    if (!agent_cfg.permissions_config.ask.empty()) {
                        permissions_json["ask"] = agent_cfg.permissions_config.ask;
                    }
                    agent_json["permissions"] = std::move(permissions_json);
                }
                agents_json[agent_key] = std::move(agent_json);
            }
            root["agents"] = std::move(agents_json);
        }

        if (permissions_config.default_mode != permission_mode::default_mode || !permissions_config.allow.empty() || !permissions_config.deny.empty() ||
            !permissions_config.ask.empty()) {
            nlohmann::json permissions_json = nlohmann::json::object();
            permissions_json["default_mode"] = std::string{magic_enum::enum_name(permissions_config.default_mode)};
            if (!permissions_config.allow.empty()) {
                permissions_json["allow"] = permissions_config.allow;
            }
            if (!permissions_config.deny.empty()) {
                permissions_json["deny"] = permissions_config.deny;
            }
            if (!permissions_config.ask.empty()) {
                permissions_json["ask"] = permissions_config.ask;
            }
            root["permissions"] = std::move(permissions_json);
        }

        try {
            if (const auto parent = path.parent_path(); !parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream file(path);
            if (!file.is_open()) {
                throw std::runtime_error("open failed");
            }
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file << root.dump(2) << '\n';
            file.close();
        } catch (const std::exception &) {
            throw std::runtime_error("Failed to write config file: " + path.string());
        }
    }

} // namespace orangutan::config
