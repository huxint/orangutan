#pragma once

#include "bootstrap/channel-serve.hpp"
#include "config/config.hpp"
#include "providers/provider.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orangutan::bootstrap::detail {

    struct ResolvedAgentEndpoints {
        providers::ProviderEndpoint primary_endpoint;
        std::vector<providers::ProviderEndpoint> fallback_endpoints;
    };

    std::unordered_map<std::string, AgentConfig> build_effective_agents(const Config &cfg);
    std::string resolve_api_key(std::string_view cli_api_key_override, const ProfileConfig &profile);
    std::optional<ResolvedAgentEndpoints> resolve_agent_endpoints(const Config &cfg, const AgentConfig &agent_cfg, std::string_view agent_key,
                                                                  std::string_view cli_api_key_override);

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, std::string_view cli_api_key_override,
                                                                                                   const CLIPermissionOptions &cli_permission_options = {});

} // namespace orangutan::bootstrap::detail
