#pragma once

#include "bootstrap/channel-serve.hpp"
#include "config/config.hpp"
#include "subagent/subagent-manager.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace orangutan::bootstrap::detail {

    struct ResolvedAgentEndpointLegacy {
        std::string profile_name;
        std::string provider_name;
        std::string api_key;
        std::string base_url;
    };

    std::unordered_map<std::string, AgentConfig> build_effective_agents(const Config &cfg);
    std::string resolve_api_key(const std::string &cli_api_key_override, const ProfileConfig &profile);
    std::optional<ResolvedAgentEndpointLegacy> resolve_agent_endpoint(const Config &cfg, const AgentConfig &agent_cfg, const std::string &agent_key,
                                                                      const std::string &cli_api_key_override);

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, const std::string &cli_api_key_override);

    std::unordered_map<std::string, SubagentChildRuntimeConfig>
    build_subagent_child_runtime_configs(const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs);

} // namespace orangutan::bootstrap::detail
