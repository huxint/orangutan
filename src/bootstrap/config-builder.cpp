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

    namespace {

        std::string provider_name_from_endpoint_style(std::string_view endpoint_style) {
            if (endpoint_style.starts_with("openai-")) {
                return "openai";
            }
            if (endpoint_style == "anthropic-messages") {
                return "anthropic";
            }
            return {};
        }

    } // namespace

    std::string resolve_api_key(const std::string &cli_api_key_override, const ProfileConfig &profile) {
        if (!cli_api_key_override.empty()) {
            return cli_api_key_override;
        }
        if (!profile.api_key.empty()) {
            return profile.api_key;
        }
        const char *env_key = std::getenv("LLM_API_KEY");
        if (env_key != nullptr) {
            return env_key;
        }
        return {};
    }

    std::unordered_map<std::string, AgentConfig> build_effective_agents(const Config &cfg) {
        auto effective_agents = cfg.agents;
        for (auto &[agent_key, agent_cfg] : effective_agents) {
            if (agent_cfg.profile.empty()) {
                agent_cfg.profile = cfg.profile;
            }
            if (agent_cfg.model.empty()) {
                agent_cfg.model = cfg.model;
            }
            if (agent_cfg.fallback_models.empty()) {
                agent_cfg.fallback_models = cfg.fallback_models;
            }
            if (agent_cfg.system_prompt.empty()) {
                agent_cfg.system_prompt = cfg.system_prompt;
            }
            if (agent_cfg.workspace.empty()) {
                agent_cfg.workspace = cfg.workspace;
            }
            if (agent_cfg.edit_mode.empty()) {
                agent_cfg.edit_mode = cfg.edit_mode;
            }
            if (agent_cfg.thinking_budget == 0) {
                agent_cfg.thinking_budget = cfg.thinking_budget;
            }
            if (agent_cfg.permissions.allowed_tools.empty() && agent_cfg.permissions.denied_tools.empty() && agent_cfg.permissions.denied_shell_commands.empty() &&
                agent_cfg.permissions.sandbox_mode == ToolSandboxMode::isolated && agent_cfg.permissions.shell_approval == ToolApprovalPolicy::ask) {
                agent_cfg.permissions = cfg.permissions;
            }
            static_cast<void>(agent_key);
        }
        for (auto &[agent_key, agent_cfg] : effective_agents) {
            if (agent_cfg.workspace.empty()) {
                agent_cfg.workspace = default_workspace_hint();
            }
            static_cast<void>(agent_key);
        }
        return effective_agents;
    }

    std::optional<ResolvedAgentEndpointLegacy> resolve_agent_endpoint(const Config &cfg, const AgentConfig &agent_cfg, const std::string &agent_key,
                                                                      const std::string &cli_api_key_override) {
        if (agent_cfg.profile.empty()) {
            spdlog::fmt_lib::println(stderr, "Error: agent '{}' is missing a profile.", agent_key);
            return std::nullopt;
        }
        const auto profile_it = cfg.profiles.find(agent_cfg.profile);
        if (profile_it == cfg.profiles.end()) {
            spdlog::fmt_lib::println(stderr, "Error: agent '{}' references unknown profile '{}'.", agent_key, agent_cfg.profile);
            return std::nullopt;
        }
        const auto &profile_cfg = profile_it->second;
        const auto model_it = profile_cfg.models.find(agent_cfg.model);
        if (model_it == profile_cfg.models.end()) {
            spdlog::fmt_lib::println(stderr, "Error: agent '{}' references unknown model '{}' in profile '{}'.", agent_key, agent_cfg.model, agent_cfg.profile);
            return std::nullopt;
        }
        const auto provider_name = provider_name_from_endpoint_style(model_it->second.endpoint_style);
        if (provider_name.empty()) {
            spdlog::fmt_lib::println(stderr, "Error: agent '{}' uses unsupported endpoint_style '{}'.", agent_key, model_it->second.endpoint_style);
            return std::nullopt;
        }
        return ResolvedAgentEndpointLegacy{
            .profile_name = agent_cfg.profile,
            .provider_name = provider_name,
            .api_key = resolve_api_key(cli_api_key_override, profile_cfg),
            .base_url = profile_cfg.base_url,
        };
    }

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, const std::string &cli_api_key_override) {
        std::unordered_map<std::string, AgentRuntimeConfig> result;
        for (const auto &[agent_key, agent_cfg] : build_effective_agents(cfg)) {
            const auto maybe_endpoint = resolve_agent_endpoint(cfg, agent_cfg, agent_key, cli_api_key_override);
            if (!maybe_endpoint.has_value()) {
                return std::nullopt;
            }

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
                                          .provider_name = maybe_endpoint->provider_name,
                                          .api_key = maybe_endpoint->api_key,
                                          .model = agent_cfg.model,
                                          .fallback_models = agent_cfg.fallback_models,
                                          .base_url = maybe_endpoint->base_url,
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
