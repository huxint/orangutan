#pragma once

#include "config/config.hpp"

#include <nlohmann/json.hpp>
#include <optional>

namespace orangutan::config::detail {

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
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
        const ConfigSecretOptions &options_;
        std::optional<std::string> cached_password_;
    };


    [[nodiscard]]
    AgentConfig make_agent_defaults(const Config &cfg);

    void expand_agent_config(AgentConfig &cfg);
    void expand_profile_config(ProfileConfig &cfg);

    void apply_permissions_config(const nlohmann::json &permissions, PermissionConfig &config);

    [[nodiscard]]
    Config parse_agent_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_profiles_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_tools_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_permissions_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_session_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_memory_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_qq_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_agents_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_qq_bots_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_security_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_skills_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_custom_tools_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_mcp_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_hooks_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_heartbeat_section(const nlohmann::json &root, Config cfg);

    [[nodiscard]]
    Config parse_json(const nlohmann::json &root, const ConfigSecretOptions &secret_options);

} // namespace orangutan::config::detail

namespace orangutan {

    namespace config_detail = config::detail;

} // namespace orangutan
