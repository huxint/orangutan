#pragma once

#include "bootstrap/cli-options.hpp"
#include "utils/transparent-lookup.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::config {
    struct AgentConfig;
    struct Config;
} // namespace orangutan::config

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::bootstrap {

    void load_display_skills(skills::SkillLoader &skill_loader, const config::Config &cfg, const std::string &workspace_root);

    [[nodiscard]]
    std::vector<std::string> resolve_runtime_hook_dirs(const std::vector<std::string> &configured_hook_paths, std::string_view workspace_root);

    void log_loaded_hooks(const std::vector<std::string> &hook_dirs, const hooks::HookManager &hook_manager);

    [[nodiscard]]
    std::optional<config::AgentConfig> resolve_selected_agent(const config::Config &cfg, const CliOptions &options);

    [[nodiscard]]
    std::optional<std::string> resolve_agent_workspace(const config::AgentConfig &selected_agent);

    [[nodiscard]]
    utils::transparent_string_unordered_map<std::string> build_qq_bot_agents(const config::Config &cfg);

} // namespace orangutan::bootstrap
