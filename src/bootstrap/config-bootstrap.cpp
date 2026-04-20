#include "bootstrap/config-bootstrap.hpp"

#include "bootstrap/config-builder.hpp"
#include "bootstrap/identity.hpp"
#include "hooks/hook-manager.hpp"
#include "skills/skill-loader.hpp"
#include "config/config.hpp"
#include "utils/path.hpp"

#include <cstdio>
#include <filesystem>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::bootstrap {

    void load_display_skills(skills::SkillLoader &skill_loader, const config::Config &cfg, const std::string &workspace_root) {
        skill_loader.set_workspace_root(std::filesystem::path{workspace_root});
        auto skill_dirs = skills::resolve_skill_directories(cfg.skill_paths, std::filesystem::path{workspace_root});
        skill_loader.load_from_directories(skill_dirs);
        const auto active_catalog = skill_loader.list(skills::skill_list_query{.include_inactive = false});
        if (!active_catalog.skills.empty()) {
            spdlog::info("loaded {} skill(s)", active_catalog.skills.size());
        }
    }

    std::vector<std::string> resolve_runtime_hook_dirs(const std::vector<std::string> &configured_hook_paths, std::string_view workspace_root) {
        if (!configured_hook_paths.empty()) {
            return configured_hook_paths;
        }

        std::vector<std::string> hook_dirs;
        if (auto home = utils::try_expand_home_path("~/.orangutan/hooks"); home.has_value()) {
            hook_dirs.push_back(home->string());
        }
        if (!workspace_root.empty()) {
            hook_dirs.push_back(workspace_hooks_root(workspace_root).string());
        }
        return hook_dirs;
    }

    void log_loaded_hooks(const std::vector<std::string> &hook_dirs, const hooks::HookManager &hook_manager) {
        for (const auto &dir : hook_dirs) {
            spdlog::info("hook directory: {}", dir);
        }
        for (const auto event : {hooks::hook_event::before_tool_call, hooks::hook_event::after_tool_call, hooks::hook_event::message_received, hooks::hook_event::message_sending,
                                 hooks::hook_event::session_start, hooks::hook_event::session_end}) {
            spdlog::info("hook count for '{}': {}", magic_enum::enum_name(event), hook_manager.hook_count(event));
        }
        spdlog::info("loaded {} hook(s) total", hook_manager.total_hooks());
    }

    std::optional<config::AgentConfig> resolve_selected_agent(const config::Config &cfg, const CliOptions &options) {
        const auto effective_agents = detail::build_effective_agents(cfg);
        const auto agent_it = effective_agents.find(options.cli_agent_key);
        if (agent_it == effective_agents.end()) {
            spdlog::fmt_lib::println(stderr, "Error: unknown agent: {}", options.cli_agent_key);
            return std::nullopt;
        }

        auto selected_agent = agent_it->second;
        if (!options.cli_model.empty()) {
            selected_agent.model = options.cli_model;
        }
        return selected_agent;
    }

    std::optional<std::string> resolve_agent_workspace(const config::AgentConfig &selected_agent) {
        try {
            auto workspace = resolve_workspace_root(selected_agent.workspace);
            if (!workspace.empty()) {
                spdlog::info("using workspace: {}", workspace);
            }
            return workspace;
        } catch (const std::exception &e) {
            spdlog::fmt_lib::println(stderr, "Error: {}", e.what());
            return std::nullopt;
        }
    }

    void apply_cli_edit_mode_override(config::Config &cfg, std::string_view edit_mode) {
        if (edit_mode.empty()) {
            return;
        }

        cfg.edit_mode = std::string(edit_mode);
        for (auto &[agent_key, agent_cfg] : cfg.agents) {
            agent_cfg.edit_mode = cfg.edit_mode;
            static_cast<void>(agent_key);
        }
    }

    utils::transparent_string_unordered_map<std::string> build_qq_bot_agents(const config::Config &cfg) {
        utils::transparent_string_unordered_map<std::string> qq_bot_agents;
        for (const auto &bot : cfg.qq_bots) {
            if (!bot.name.empty()) {
                qq_bot_agents.insert_or_assign(bot.name, bot.agent);
            }
        }
        return qq_bot_agents;
    }

} // namespace orangutan::bootstrap
