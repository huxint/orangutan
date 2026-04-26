#include "bootstrap/config-builder.hpp"

#include "bootstrap/identity.hpp"

#include <filesystem>
#include <fmt/format.h>
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

    orangutan::ToolPermissionContext build_agent_permission_context(const orangutan::config::AgentConfig &agent_cfg, const orangutan::CLIPermissionOptions &cli_permission_options,
                                                                    std::string_view workspace_root) {
        return orangutan::initialize_permission_context(agent_cfg.permissions_config, cli_permission_options, workspace_root);
    }

} // namespace

namespace orangutan::bootstrap::detail {

    std::string resolve_api_key(std::string_view cli_api_key_override, const ProfileConfig &profile) {
        if (!cli_api_key_override.empty()) {
            return std::string{cli_api_key_override};
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
            if (agent_cfg.workspace.empty()) {
                agent_cfg.workspace = cfg.workspace;
            }
            if (agent_cfg.thinking_budget == 0) {
                agent_cfg.thinking_budget = cfg.thinking_budget;
            }
            if (agent_cfg.permissions_config.allow.empty() && agent_cfg.permissions_config.deny.empty() && agent_cfg.permissions_config.ask.empty() &&
                agent_cfg.permissions_config.default_mode == permission_mode::default_mode) {
                agent_cfg.permissions_config = cfg.permissions_config;
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

    std::optional<ResolvedAgentRoute> resolve_agent_route(const Config &cfg, const AgentConfig &agent_cfg, std::string_view agent_key, std::string_view cli_api_key_override) {
        if (agent_cfg.profile.empty()) {
            fmt::println(stderr, "Error: agent '{}' is missing a profile.", agent_key);
            return std::nullopt;
        }
        const auto profile_it = cfg.profiles.find(agent_cfg.profile);
        if (profile_it == cfg.profiles.end()) {
            fmt::println(stderr, "Error: agent '{}' references unknown profile '{}'.", agent_key, agent_cfg.profile);
            return std::nullopt;
        }
        const auto &profile_cfg = profile_it->second;
        const auto model_it = profile_cfg.models.find(agent_cfg.model);
        if (model_it == profile_cfg.models.end()) {
            fmt::println(stderr, "Error: agent '{}' references unknown model '{}' in profile '{}'.", agent_key, agent_cfg.model, agent_cfg.profile);
            return std::nullopt;
        }

        ResolvedAgentRoute resolved;
        try {
            resolved.route.primary = providers::ModelTarget{
                .profile_name = agent_cfg.profile,
                .model = agent_cfg.model,
                .base_url = profile_cfg.base_url,
                .api_key = resolve_api_key(cli_api_key_override, profile_cfg),
                .headers = profile_cfg.headers,
                .default_max_tokens = model_it->second.max_tokens,
                .provider = providers::parse_provider_kind(model_it->second.provider),
                .protocol = providers::parse_protocol_kind(model_it->second.protocol),
                .thinking = model_it->second.thinking,
            };
        } catch (const providers::ProviderError &error) {
            fmt::println(stderr, "Error: agent '{}' model '{}' has invalid provider config: {}", agent_key, agent_cfg.model, error.what());
            return std::nullopt;
        }

        for (const auto &fallback_model : agent_cfg.fallback_models) {
            const auto fallback_profile_name = fallback_model.profile.empty() ? agent_cfg.profile : fallback_model.profile;
            if (fallback_model.model.empty() || (fallback_profile_name == agent_cfg.profile && fallback_model.model == agent_cfg.model)) {
                continue;
            }
            const auto fallback_profile_it = cfg.profiles.find(fallback_profile_name);
            if (fallback_profile_it == cfg.profiles.end()) {
                fmt::println(stderr, "Error: agent '{}' fallback profile '{}' is not defined.", agent_key, fallback_profile_name);
                return std::nullopt;
            }
            const auto &fallback_profile_cfg = fallback_profile_it->second;
            const auto fallback_it = fallback_profile_cfg.models.find(fallback_model.model);
            if (fallback_it == fallback_profile_cfg.models.end()) {
                fmt::println(stderr, "Error: agent '{}' fallback model '{}' is not defined in profile '{}'.", agent_key, fallback_model.model, fallback_profile_name);
                return std::nullopt;
            }
            try {
                resolved.route.fallbacks.push_back(providers::ModelTarget{
                    .profile_name = fallback_profile_name,
                    .model = fallback_model.model,
                    .base_url = fallback_profile_cfg.base_url,
                    .api_key = resolve_api_key(cli_api_key_override, fallback_profile_cfg),
                    .headers = fallback_profile_cfg.headers,
                    .default_max_tokens = fallback_it->second.max_tokens,
                    .provider = providers::parse_provider_kind(fallback_it->second.provider),
                    .protocol = providers::parse_protocol_kind(fallback_it->second.protocol),
                    .thinking = fallback_it->second.thinking,
                });
            } catch (const providers::ProviderError &error) {
                fmt::println(stderr, "Error: agent '{}' fallback model '{}' has invalid provider config: {}", agent_key, fallback_model.model, error.what());
                return std::nullopt;
            }
        }

        return resolved;
    }

    std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const Config &cfg, std::string_view cli_api_key_override,
                                                                                                   const CLIPermissionOptions &cli_permission_options) {
        std::unordered_map<std::string, AgentRuntimeConfig> result;
        for (const auto &[agent_key, agent_cfg] : build_effective_agents(cfg)) {
            const auto maybe_route = resolve_agent_route(cfg, agent_cfg, agent_key, cli_api_key_override);
            if (!maybe_route.has_value()) {
                return std::nullopt;
            }

            std::string resolved_workspace_root;
            try {
                resolved_workspace_root = resolve_workspace_root(agent_cfg.workspace);
            } catch (const std::exception &e) {
                fmt::println(stderr, "Error: failed to resolve workspace for agent '{}': {}", agent_key, e.what());
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
                                          .provider_route = maybe_route->route,
                                          .api_key_override = std::string{cli_api_key_override},
                                          .workspace_root = resolved_workspace_root,
                                          .thinking_budget = agent_cfg.thinking_budget,
                                          .cli_runtime_key = cli_identity.runtime_key,
                                          .cli_memory_scope = cli_identity.memory_scope,
                                          .memory = cfg.memory,
                                          .permission_context = build_agent_permission_context(agent_cfg, cli_permission_options, resolved_workspace_root),
                                          .leader_mode = agent_cfg.leader_mode,
                                          .max_concurrent_agents = agent_cfg.max_concurrent_agents,
                                      });
        }
        return result;
    }

} // namespace orangutan::bootstrap::detail
