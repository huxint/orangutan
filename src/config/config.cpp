#include "config/config-detail.hpp"
#include "config/secret-fields.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace orangutan::config {

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
                spdlog::warn("Environment variable '{}' is not set, replacing with empty string", var_name);
            }

            pos = end + 1;
        }

        return result;
    }

    std::string expand_home_path(const std::string &input) {
        if (input.empty() || input[0] != '~') {
            return input;
        }

        if (input.size() > 1 && input[1] != '/') {
            spdlog::warn("Path '{}' uses unsupported ~user syntax; leaving it unchanged", input);
            return input;
        }

        const char *home = std::getenv("HOME");
        if (home == nullptr) {
            spdlog::warn("HOME is not set; cannot expand path '{}'", input);
            return input;
        }

        if (input == "~") {
            return home;
        }

        return std::string{home} + input.substr(1);
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
            Config cfg;
            cfg = parse_agent_section(root, std::move(cfg));
            cfg = parse_profiles_section(root, std::move(cfg));
            cfg = parse_tools_section(root, std::move(cfg));
            cfg = parse_permissions_section(root, std::move(cfg));
            cfg = parse_session_section(root, std::move(cfg));
            cfg = parse_memory_section(root, std::move(cfg));
            cfg = parse_qq_section(root, std::move(cfg));

            ConfigPasswordResolver password_resolver(secret_options);

            cfg.profile = expand_env_vars(cfg.profile);
            cfg.model = expand_env_vars(cfg.model);
            cfg.system_prompt = expand_env_vars(cfg.system_prompt);
            cfg.workspace = expand_home_path(expand_env_vars(cfg.workspace));
            for (auto &fallback_model : cfg.fallback_models) {
                fallback_model.profile = expand_env_vars(fallback_model.profile);
                fallback_model.model = expand_env_vars(fallback_model.model);
            }
            for (auto &[profile_name, profile_cfg] : cfg.profiles) {
                const auto display_field = "profiles." + profile_name + ".api_key";
                resolve_secret_field(profile_cfg.api_key, profile_api_key_field().field_kind, display_field, password_resolver);
                expand_profile_config(profile_cfg);
            }
            cfg.memory.mirror_file = expand_home_path(expand_env_vars(cfg.memory.mirror_file));
            cfg.memory.journal_dir = expand_home_path(expand_env_vars(cfg.memory.journal_dir));
            cfg.qq_app_id = expand_env_vars(cfg.qq_app_id);
            resolve_secret_field(cfg.qq_client_secret, qq_client_secret_field().field_kind, "qq.client_secret", password_resolver);

            cfg = parse_agents_section(root, std::move(cfg));
            cfg = parse_qq_bots_section(root, std::move(cfg));
            cfg = parse_security_section(root, std::move(cfg));
            cfg = parse_skills_section(root, std::move(cfg));
            cfg = parse_custom_tools_section(root, std::move(cfg));
            cfg = parse_mcp_section(root, std::move(cfg));
            cfg = parse_hooks_section(root, std::move(cfg));
            cfg = parse_heartbeat_section(root, std::move(cfg));

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

            for (std::size_t index = 0; index < cfg.qq_bots.size(); ++index) {
                auto &bot = cfg.qq_bots[index];
                bot.name = expand_env_vars(bot.name);
                bot.app_id = expand_env_vars(bot.app_id);
                const auto display_field = "qq_bots[" + std::to_string(index) + "].client_secret";
                resolve_secret_field(bot.client_secret, qq_bot_client_secret_field().field_kind, display_field, password_resolver);
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
            spdlog::warn("Failed to parse config file {}: {}", path.string(), e.what());
            return {};
        } catch (const nlohmann::json::type_error &e) {
            spdlog::warn("Invalid config file {}: {}", path.string(), e.what());
            return {};
        } catch (const std::runtime_error &e) {
            spdlog::warn("Invalid config file {}: {}", path.string(), e.what());
            return {};
        }
    }

    bool Config::is_tool_allowed(const std::string &name) const {
        auto effective = permissions;
        for (const auto &tool : allowed_tools) {
            if (!std::ranges::contains(effective.allowed_tools, tool)) {
                effective.allowed_tools.push_back(tool);
            }
        }
        for (const auto &tool : denied_tools) {
            if (!std::ranges::contains(effective.denied_tools, tool)) {
                effective.denied_tools.push_back(tool);
            }
        }
        return orangutan::is_tool_allowed(effective, name);
    }

    std::optional<AgentConfig> Config::find_agent(const std::string &key) const {
        if (const auto it = agents.find(key); it != agents.end()) {
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
        if (!system_prompt.empty()) {
            agent["system_prompt"] = system_prompt;
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
                    model_json["endpoint_style"] = model_cfg.endpoint_style;
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
        if (!allowed_tools.empty()) {
            tools["allowed"] = allowed_tools;
        }
        if (!denied_tools.empty()) {
            tools["denied"] = denied_tools;
        }
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
                if (!agent_cfg.system_prompt.empty()) {
                    agent_json["system_prompt"] = agent_cfg.system_prompt;
                }
                if (!agent_cfg.workspace.empty()) {
                    agent_json["workspace"] = agent_cfg.workspace;
                }
                if (!agent_cfg.subagents.empty()) {
                    agent_json["subagents"] = agent_cfg.subagents;
                }
                if (!agent_cfg.edit_mode.empty() && agent_cfg.edit_mode != "hashline") {
                    agent_json["edit_mode"] = agent_cfg.edit_mode;
                }
                if (agent_cfg.thinking_budget != 0) {
                    agent_json["thinking_budget"] = agent_cfg.thinking_budget;
                }
                if (agent_cfg.permissions.sandbox_mode != ToolSandboxMode::isolated || agent_cfg.permissions.shell_approval != ToolApprovalPolicy::ask ||
                    !agent_cfg.permissions.allowed_tools.empty() || !agent_cfg.permissions.denied_tools.empty() || !agent_cfg.permissions.denied_shell_commands.empty()) {
                    nlohmann::json permissions_json = nlohmann::json::object();
                    permissions_json["sandbox_mode"] = to_string(agent_cfg.permissions.sandbox_mode);
                    permissions_json["shell_approval_policy"] = to_string(agent_cfg.permissions.shell_approval);
                    if (!agent_cfg.permissions.allowed_tools.empty()) {
                        permissions_json["allowed_tools"] = agent_cfg.permissions.allowed_tools;
                    }
                    if (!agent_cfg.permissions.denied_tools.empty()) {
                        permissions_json["denied_tools"] = agent_cfg.permissions.denied_tools;
                    }
                    if (!agent_cfg.permissions.denied_shell_commands.empty()) {
                        permissions_json["denied_shell_commands"] = agent_cfg.permissions.denied_shell_commands;
                    }
                    agent_json["permissions"] = std::move(permissions_json);
                }
                agents_json[agent_key] = std::move(agent_json);
            }
            root["agents"] = std::move(agents_json);
        }

        if (permissions.sandbox_mode != ToolSandboxMode::isolated || permissions.shell_approval != ToolApprovalPolicy::ask || !permissions.allowed_tools.empty() ||
            !permissions.denied_tools.empty() || !permissions.denied_shell_commands.empty()) {
            nlohmann::json permissions_json = nlohmann::json::object();
            permissions_json["sandbox_mode"] = to_string(permissions.sandbox_mode);
            permissions_json["shell_approval_policy"] = to_string(permissions.shell_approval);
            if (!permissions.allowed_tools.empty()) {
                permissions_json["allowed_tools"] = permissions.allowed_tools;
            }
            if (!permissions.denied_tools.empty()) {
                permissions_json["denied_tools"] = permissions.denied_tools;
            }
            if (!permissions.denied_shell_commands.empty()) {
                permissions_json["denied_shell_commands"] = permissions.denied_shell_commands;
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
