#pragma once

#include "infra/config/config.hpp"

#include <optional>

#include <toml++/toml.hpp>

namespace orangutan::config_detail {

class ConfigPasswordResolver {
public:
    explicit ConfigPasswordResolver(const ConfigSecretOptions &options)
    : options_(options) {}

    [[nodiscard]]
    const std::string &resolve() {
        if (!cached_password_.has_value()) {
            cached_password_ = resolve_config_secret_password(options_);
        }
        return *cached_password_;
    }

private:
    const ConfigSecretOptions &options_;
    std::optional<std::string> cached_password_;
};

void resolve_secret_field(std::string &value, std::string_view field_kind, std::string_view display_field, ConfigPasswordResolver &resolver);

[[nodiscard]]
AgentConfig make_agent_config_from_legacy(const Config &cfg);

void expand_agent_config(AgentConfig &cfg);

void apply_permissions_table(const toml::table &permissions, ToolPermissionSettings &settings);

[[nodiscard]]
Config parse_agent_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_tools_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_permissions_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_session_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_memory_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_qq_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_agents_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_qq_bots_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_security_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_skills_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_custom_tools_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_mcp_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_hooks_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_heartbeat_section(const toml::table &tbl, Config cfg);

[[nodiscard]]
Config parse_toml(const toml::table &tbl, const ConfigSecretOptions &secret_options);

} // namespace orangutan::config_detail
