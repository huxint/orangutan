#pragma once

#include "permissions/permission-types.hpp"
#include "providers/provider.hpp"

#include <string>
#include <vector>

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig {
        std::string agent_key;
        std::string model;
        std::vector<std::string> fallback_models;
        providers::ProviderRoute provider_route;
        std::string api_key_override;
        std::string workspace_root;
        int thinking_budget = 0;
        std::string cli_runtime_key;
        std::string cli_memory_scope;
        ToolPermissionContext permission_context;
        bool leader_mode = false;
        int max_concurrent_agents = 4;
    };

} // namespace orangutan::bootstrap
