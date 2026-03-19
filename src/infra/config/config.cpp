#include "infra/config/config-detail.hpp"
#include "infra/config/secret-fields.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

namespace orangutan {

std::string expand_env_vars(const std::string &input) {
    std::string result;
    result.reserve(input.size());

    size_t pos = 0;
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

    return std::string(home) + input.substr(1);
}

namespace config_detail {

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

Config parse_toml(const toml::table &tbl, const ConfigSecretOptions &secret_options) {
    Config cfg;
    cfg = parse_agent_section(tbl, std::move(cfg));
    cfg = parse_tools_section(tbl, std::move(cfg));
    cfg = parse_permissions_section(tbl, std::move(cfg));
    cfg = parse_session_section(tbl, std::move(cfg));
    cfg = parse_memory_section(tbl, std::move(cfg));
    cfg = parse_qq_section(tbl, std::move(cfg));

    ConfigPasswordResolver password_resolver(secret_options);

    resolve_secret_field(cfg.api_key, legacy_agent_api_key_field().field_kind, "agent.api_key", password_resolver);
    cfg.base_url = expand_env_vars(cfg.base_url);
    cfg.model = expand_env_vars(cfg.model);
    cfg.provider = expand_env_vars(cfg.provider);
    cfg.system_prompt = expand_env_vars(cfg.system_prompt);
    cfg.workspace = expand_home_path(expand_env_vars(cfg.workspace));
    for (auto &fallback_model : cfg.fallback_models) {
        fallback_model = expand_env_vars(fallback_model);
    }
    cfg.memory.mirror_file = expand_home_path(expand_env_vars(cfg.memory.mirror_file));
    cfg.memory.journal_dir = expand_home_path(expand_env_vars(cfg.memory.journal_dir));
    cfg.qq_app_id = expand_env_vars(cfg.qq_app_id);
    resolve_secret_field(cfg.qq_client_secret, qq_client_secret_field().field_kind, "qq.client_secret", password_resolver);

    cfg = parse_agents_section(tbl, std::move(cfg));
    cfg = parse_qq_bots_section(tbl, std::move(cfg));
    cfg = parse_security_section(tbl, std::move(cfg));
    cfg = parse_skills_section(tbl, std::move(cfg));
    cfg = parse_custom_tools_section(tbl, std::move(cfg));
    cfg = parse_mcp_section(tbl, std::move(cfg));
    cfg = parse_hooks_section(tbl, std::move(cfg));
    cfg = parse_heartbeat_section(tbl, std::move(cfg));

    if (cfg.agents.empty() || !cfg.agents.contains("default")) {
        cfg.agents.insert_or_assign("default", make_agent_config_from_legacy(cfg));
    }

    for (auto &[key, agent_cfg] : cfg.agents) {
        const auto display_field = "agents." + key + ".api_key";
        resolve_secret_field(agent_cfg.api_key, named_agent_api_key_field().field_kind, display_field, password_resolver);
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

    for (size_t index = 0; index < cfg.qq_bots.size(); ++index) {
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

} // namespace config_detail

Config Config::load(const ConfigSecretOptions &secret_options) {
    auto path = default_orangutan_config_path();
    if (path.empty() || !std::filesystem::exists(path)) {
        return {};
    }
    return load_from(path, secret_options);
}

Config Config::load_from(const std::string &path, const ConfigSecretOptions &secret_options) {
    try {
        auto tbl = toml::parse_file(path);
        return config_detail::parse_toml(tbl, secret_options);
    } catch (const ConfigSecretProtectionError &) {
        throw;
    } catch (const toml::parse_error &e) {
        spdlog::warn("Failed to parse config file {}: {}", path, e.what());
        return {};
    } catch (const std::runtime_error &e) {
        spdlog::warn("Invalid config file {}: {}", path, e.what());
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

} // namespace orangutan
