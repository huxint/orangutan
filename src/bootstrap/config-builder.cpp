#include "bootstrap/config-builder.hpp"

#include "bootstrap/identity.hpp"

#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>

namespace {

    std::string default_workspace_hint() {
        const char *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            return {};
        }
        return (std::filesystem::path(home) / ".orangutan" / "workspace" / "main").lexically_normal().string();
    }

} // namespace

namespace orangutan::bootstrap::detail {

    std::string resolve_api_key(const std::string &cli_api_key_override, const Config &cfg) {
        if (!cli_api_key_override.empty()) {
            return cli_api_key_override;
        }
        const char *env_key = std::getenv("ANTHROPIC_API_KEY");
        if (env_key == nullptr) {
            env_key = std::getenv("LLM_API_KEY");
        }
        if (env_key != nullptr) {
            return env_key;
        }
        if (!cfg.api_key.empty()) {
            return cfg.api_key;
        }
        return {};
    }

    std::unordered_map<std::string, AgentConfig> build_effective_agents(const Config &cfg) {
        auto effective_agents = cfg.agents;
        if (!effective_agents.contains("default")) {
            effective_agents.insert_or_assign("default", AgentConfig{
                                                             .provider = cfg.provider,
                                                             .model = cfg.model,
                                                             .fallback_models = cfg.fallback_models,
                                                             .base_url = cfg.base_url,
                                                             .api_key = cfg.api_key,
                                                             .system_prompt = cfg.system_prompt,
                                                             .workspace = cfg.workspace,
                                                             .permissions = cfg.permissions,
                                                             .subagents = {},
                                                             .edit_mode = cfg.edit_mode,
                                                             .thinking_budget = cfg.thinking_budget,
                                                         });
        }
        for (auto &[agent_key, agent_cfg] : effective_agents) {
            if (agent_cfg.workspace.empty()) {
                agent_cfg.workspace = default_workspace_hint();
            }
            static_cast<void>(agent_key);
        }
        return effective_agents;
    }

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, const std::string &cli_api_key_override) {
        std::unordered_map<std::string, AgentRuntimeConfig> result;
        for (const auto &[agent_key, agent_cfg] : build_effective_agents(cfg)) {
            Config agent_cfg_wrapper = cfg;
            agent_cfg_wrapper.api_key = agent_cfg.api_key;
            const auto resolved_agent_api_key = resolve_api_key(cli_api_key_override, agent_cfg_wrapper);

            std::string resolved_workspace_root;
            try {
                resolved_workspace_root = resolve_workspace_root(agent_cfg.workspace);
            } catch (const std::exception &e) {
                spdlog::fmt_lib::println(stderr, "Error: failed to resolve workspace for agent '{}': {}", agent_key, e.what());
                return std::nullopt;
            }

            const auto cli_identity = derive_cli_identity(resolved_workspace_root, agent_key);

            result.emplace(agent_key, AgentRuntimeConfig{
                                          .agent_key = agent_key,
                                          .provider_name = agent_cfg.provider,
                                          .api_key = resolved_agent_api_key,
                                          .model = agent_cfg.model,
                                          .fallback_models = agent_cfg.fallback_models,
                                          .base_url = agent_cfg.base_url,
                                          .system_prompt = agent_cfg.system_prompt,
                                          .workspace_root = resolved_workspace_root,
                                          .edit_mode = agent_cfg.edit_mode,
                                          .thinking_budget = agent_cfg.thinking_budget,
                                          .cli_runtime_key = cli_identity.runtime_key,
                                          .cli_memory_scope = cli_identity.memory_scope,
                                          .memory = cfg.memory,
                                          .permissions = agent_cfg.permissions,
                                          .allowed_child_agents = agent_cfg.subagents,
                                      });
        }
        return result;
    }

    std::unordered_map<std::string, SubagentChildRuntimeConfig>
    build_subagent_child_runtime_configs(const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs) {
        std::unordered_map<std::string, SubagentChildRuntimeConfig> result;
        for (const auto &[agent_key, runtime_cfg] : agent_runtime_configs) {
            result.emplace(agent_key, SubagentChildRuntimeConfig{
                                          .agent_key = runtime_cfg.agent_key,
                                          .provider_name = runtime_cfg.provider_name,
                                          .api_key = runtime_cfg.api_key,
                                          .model = runtime_cfg.model,
                                          .fallback_models = runtime_cfg.fallback_models,
                                          .base_url = runtime_cfg.base_url,
                                          .system_prompt = runtime_cfg.system_prompt,
                                          .workspace_root = runtime_cfg.workspace_root,
                                          .edit_mode = runtime_cfg.edit_mode,
                                          .thinking_budget = runtime_cfg.thinking_budget,
                                          .memory = runtime_cfg.memory,
                                          .permissions = runtime_cfg.permissions,
                                          .allowed_child_agents = runtime_cfg.allowed_child_agents,
                                      });
        }
        return result;
    }

} // namespace orangutan::bootstrap::detail
