#pragma once

#include "app/channel-serve.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/config/config.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace orangutan::app::detail {

std::optional<std::unordered_map<std::string, AgentRuntimeConfig>>
build_agent_runtime_configs(const orangutan::Config &cfg, const std::string &cli_api_key_override);

std::unordered_map<std::string, SubagentChildRuntimeConfig>
build_subagent_child_runtime_configs(const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs);

} // namespace orangutan::app::detail
