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
        return (std::filesystem::path(home) / "workspace").lexically_normal().string();
    }

    std::string fallback_display_label(const orangutan::config::FallbackModelRef &fallback) {
        if (fallback.profile.empty()) {
            return fallback.model;
        }
        return fallback.profile + ":" + fallback.model;
    }

} // namespace

namespace orangutan::bootstrap::detail {

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

    std::optional<ResolvedAgentEndpoints> resolve_agent_endpoints(const Config &cfg, const AgentConfig &agent_cfg, const std::string &agent_key,
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

        ResolvedAgentEndpoints resolved{
            .primary_endpoint =
                providers::ProviderEndpoint{
                    .profile_name = agent_cfg.profile,
                    .endpoint_style = model_it->second.endpoint_style,
                    .api_key = resolve_api_key(cli_api_key_override, profile_cfg),
                    .model = agent_cfg.model,
                    .base_url = profile_cfg.base_url,
                    .headers = profile_cfg.headers,
                    .default_max_tokens = model_it->second.max_tokens,
                    .thinking = model_it->second.thinking,
                },
        };

        for (const auto &fallback_model : agent_cfg.fallback_models) {
            const auto fallback_profile_name = fallback_model.profile.empty() ? agent_cfg.profile : fallback_model.profile;
            if (fallback_model.model.empty() || (fallback_profile_name == agent_cfg.profile && fallback_model.model == agent_cfg.model)) {
                continue;
            }
            const auto fallback_profile_it = cfg.profiles.find(fallback_profile_name);
            if (fallback_profile_it == cfg.profiles.end()) {
                spdlog::fmt_lib::println(stderr, "Error: agent '{}' fallback profile '{}' is not defined.", agent_key, fallback_profile_name);
                return std::nullopt;
            }
            const auto &fallback_profile_cfg = fallback_profile_it->second;
            const auto fallback_it = fallback_profile_cfg.models.find(fallback_model.model);
            if (fallback_it == fallback_profile_cfg.models.end()) {
                spdlog::fmt_lib::println(stderr, "Error: agent '{}' fallback model '{}' is not defined in profile '{}'.", agent_key, fallback_model.model, fallback_profile_name);
                return std::nullopt;
            }
            resolved.fallback_endpoints.push_back(providers::ProviderEndpoint{
                .profile_name = fallback_profile_name,
                .endpoint_style = fallback_it->second.endpoint_style,
                .api_key = resolve_api_key(cli_api_key_override, fallback_profile_cfg),
                .model = fallback_model.model,
                .base_url = fallback_profile_cfg.base_url,
                .headers = fallback_profile_cfg.headers,
                .default_max_tokens = fallback_it->second.max_tokens,
                .thinking = fallback_it->second.thinking,
            });
        }

        return resolved;
    }

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, const std::string &cli_api_key_override) {
        std::unordered_map<std::string, AgentRuntimeConfig> result;
        for (const auto &[agent_key, agent_cfg] : build_effective_agents(cfg)) {
            const auto maybe_endpoints = resolve_agent_endpoints(cfg, agent_cfg, agent_key, cli_api_key_override);
            if (!maybe_endpoints.has_value()) {
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
                                          .model = agent_cfg.model,
                                          .fallback_models =
                                              [&] {
                                                  std::vector<std::string> labels;
                                                  labels.reserve(agent_cfg.fallback_models.size());
                                                  for (const auto &fallback : agent_cfg.fallback_models) {
                                                      labels.push_back(fallback_display_label(fallback));
                                                  }
                                                  return labels;
                                              }(),
                                          .primary_endpoint = maybe_endpoints->primary_endpoint,
                                          .fallback_endpoints = maybe_endpoints->fallback_endpoints,
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
                                          .model = runtime_cfg.model,
                                          .fallback_models = runtime_cfg.fallback_models,
                                          .primary_endpoint = runtime_cfg.primary_endpoint,
                                          .fallback_endpoints = runtime_cfg.fallback_endpoints,
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
