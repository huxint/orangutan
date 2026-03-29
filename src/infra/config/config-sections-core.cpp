#include "infra/config/config-detail.hpp"

#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::config_detail {

    AgentConfig make_agent_config_from_legacy(const Config &cfg) {
        return {
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
        };
    }

    void expand_agent_config(AgentConfig &cfg) {
        cfg.base_url = expand_env_vars(cfg.base_url);
        cfg.model = expand_env_vars(cfg.model);
        cfg.provider = expand_env_vars(cfg.provider);
        cfg.system_prompt = expand_env_vars(cfg.system_prompt);
        cfg.workspace = expand_home_path(expand_env_vars(cfg.workspace));

        for (auto &fallback_model : cfg.fallback_models) {
            fallback_model = expand_env_vars(fallback_model);
        }

        for (auto &subagent : cfg.subagents) {
            subagent = expand_env_vars(subagent);
        }
    }

    Config parse_agent_section(const toml::table &tbl, Config cfg) {
        const auto *agent = tbl["agent"].as_table();
        if (agent == nullptr) {
            return cfg;
        }

        if (auto v = (*agent)["provider"].value<std::string>()) {
            cfg.provider = *v;
        }
        if (auto v = (*agent)["model"].value<std::string>()) {
            cfg.model = *v;
        }
        if (const auto *arr = (*agent)["fallback_models"].as_array()) {
            cfg.fallback_models.clear();
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.fallback_models.push_back(*s);
                }
            }
        }
        if (auto v = (*agent)["base_url"].value<std::string>()) {
            cfg.base_url = *v;
        }
        if (auto v = (*agent)["temperature"].value<base::f64>()) {
            cfg.temperature = *v;
        }
        if (auto v = (*agent)["max_iterations"].value<int64_t>()) {
            cfg.max_iterations = static_cast<int>(*v);
        }
        if (auto v = (*agent)["max_tokens"].value<int64_t>()) {
            cfg.max_tokens = static_cast<int>(*v);
        }
        if (auto v = (*agent)["thinking_budget"].value<int64_t>()) {
            cfg.thinking_budget = static_cast<int>(*v);
        }
        if (auto v = (*agent)["workspace"].value<std::string>()) {
            cfg.workspace = *v;
        }
        if (auto v = (*agent)["system_prompt"].value<std::string>()) {
            cfg.system_prompt = *v;
        }
        if (auto v = (*agent)["api_key"].value<std::string>()) {
            cfg.api_key = *v;
        }

        return cfg;
    }

    Config parse_tools_section(const toml::table &tbl, Config cfg) {
        const auto *tools = tbl["tools"].as_table();
        if (tools == nullptr) {
            return cfg;
        }

        if (const auto *arr = (*tools)["allowed"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.allowed_tools.push_back(*s);
                    cfg.permissions.allowed_tools.push_back(*s);
                }
            }
        }
        if (const auto *arr = (*tools)["denied"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.denied_tools.push_back(*s);
                    cfg.permissions.denied_tools.push_back(*s);
                }
            }
        }

        if (auto mode = (*tools)["edit_mode"].value<std::string>()) {
            cfg.edit_mode = *mode;
        }

        return cfg;
    }

    void apply_permissions_table(const toml::table &permissions, ToolPermissionSettings &settings) {
        if (auto v = permissions["sandbox_mode"].value<std::string>()) {
            const auto expanded = expand_env_vars(*v);
            if (const auto parsed = parse_tool_sandbox_mode(expanded); parsed.has_value()) {
                settings.sandbox_mode = *parsed;
            } else {
                spdlog::warn("Unknown permissions.sandbox_mode '{}', keeping default '{}'", expanded, to_string(settings.sandbox_mode));
            }
        }

        if (auto v = permissions["shell_approval_policy"].value<std::string>()) {
            const auto expanded = expand_env_vars(*v);
            if (const auto parsed = parse_tool_approval_policy(expanded); parsed.has_value()) {
                settings.shell_approval = *parsed;
            } else {
                spdlog::warn("Unknown permissions.shell_approval_policy '{}', keeping default '{}'", expanded, to_string(settings.shell_approval));
            }
        }

        if (const auto *arr = permissions["allowed_tools"].as_array()) {
            settings.allowed_tools.clear();
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    settings.allowed_tools.push_back(expand_env_vars(*s));
                }
            }
        }

        if (const auto *arr = permissions["denied_tools"].as_array()) {
            settings.denied_tools.clear();
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    settings.denied_tools.push_back(expand_env_vars(*s));
                }
            }
        }

        if (const auto *arr = permissions["denied_shell_commands"].as_array()) {
            settings.denied_shell_commands.clear();
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    settings.denied_shell_commands.push_back(expand_env_vars(*s));
                }
            }
        }
    }

    Config parse_permissions_section(const toml::table &tbl, Config cfg) {
        const auto *permissions = tbl["permissions"].as_table();
        if (permissions == nullptr) {
            return cfg;
        }

        apply_permissions_table(*permissions, cfg.permissions);
        return cfg;
    }

    Config parse_session_section(const toml::table &tbl, Config cfg) {
        const auto *session = tbl["session"].as_table();
        if (session == nullptr) {
            return cfg;
        }

        if (auto v = (*session)["auto_save"].value<bool>()) {
            cfg.auto_save = *v;
        }

        return cfg;
    }

    Config parse_memory_section(const toml::table &tbl, Config cfg) {
        const auto *memory = tbl["memory"].as_table();
        if (memory == nullptr) {
            return cfg;
        }

        if (auto v = (*memory)["mirror_enabled"].value<bool>()) {
            cfg.memory.mirror_enabled = *v;
        }
        if (auto v = (*memory)["mirror_file"].value<std::string>()) {
            cfg.memory.mirror_file = *v;
        }
        if (auto v = (*memory)["journal_dir"].value<std::string>()) {
            cfg.memory.journal_dir = *v;
        }

        return cfg;
    }

    Config parse_qq_section(const toml::table &tbl, Config cfg) {
        const auto *qq = tbl["qq"].as_table();
        if (qq == nullptr) {
            return cfg;
        }

        if (auto v = (*qq)["app_id"].value<std::string>()) {
            cfg.qq_app_id = *v;
        }
        if (auto v = (*qq)["client_secret"].value<std::string>()) {
            cfg.qq_client_secret = *v;
        }

        return cfg;
    }

    Config parse_agents_section(const toml::table &tbl, Config cfg) {
        const auto *agents = tbl["agents"].as_table();
        if (agents == nullptr) {
            return cfg;
        }

        const auto legacy_defaults = make_agent_config_from_legacy(cfg);
        for (const auto &[key_node, value_node] : *agents) {
            const auto key = std::string(key_node.str());
            const auto *agent = value_node.as_table();
            if (agent == nullptr) {
                continue;
            }

            auto agent_cfg = legacy_defaults;
            if (auto v = (*agent)["provider"].value<std::string>()) {
                agent_cfg.provider = *v;
            }
            if (auto v = (*agent)["model"].value<std::string>()) {
                agent_cfg.model = *v;
            }
            if (const auto *arr = (*agent)["fallback_models"].as_array()) {
                agent_cfg.fallback_models.clear();
                for (const auto &item : *arr) {
                    if (auto s = item.value<std::string>()) {
                        agent_cfg.fallback_models.push_back(*s);
                    }
                }
            }
            if (auto v = (*agent)["base_url"].value<std::string>()) {
                agent_cfg.base_url = *v;
            }
            if (auto v = (*agent)["api_key"].value<std::string>()) {
                agent_cfg.api_key = *v;
            }
            if (auto v = (*agent)["system_prompt"].value<std::string>()) {
                agent_cfg.system_prompt = *v;
            }
            if (auto v = (*agent)["workspace"].value<std::string>()) {
                agent_cfg.workspace = *v;
            }
            if (const auto *permissions = (*agent)["permissions"].as_table()) {
                apply_permissions_table(*permissions, agent_cfg.permissions);
            }
            agent_cfg.subagents.clear();
            if (const auto *arr = (*agent)["subagents"].as_array()) {
                for (const auto &item : *arr) {
                    if (auto s = item.value<std::string>()) {
                        agent_cfg.subagents.push_back(*s);
                    }
                }
            }
            if (auto v = (*agent)["edit_mode"].value<std::string>()) {
                agent_cfg.edit_mode = *v;
            }
            if (auto v = (*agent)["thinking_budget"].value<int64_t>()) {
                agent_cfg.thinking_budget = static_cast<int>(*v);
            }

            cfg.agents.insert_or_assign(key, std::move(agent_cfg));
        }

        return cfg;
    }

    Config parse_qq_bots_section(const toml::table &tbl, Config cfg) {
        const auto *bots = tbl["qq_bots"].as_array();
        if (bots == nullptr) {
            return cfg;
        }

        for (const auto &item : *bots) {
            const auto *bot = item.as_table();
            if (bot == nullptr) {
                continue;
            }

            QqBotConfig bot_cfg;
            if (auto v = (*bot)["name"].value<std::string>()) {
                bot_cfg.name = *v;
            }
            if (auto v = (*bot)["app_id"].value<std::string>()) {
                bot_cfg.app_id = *v;
            }
            if (auto v = (*bot)["client_secret"].value<std::string>()) {
                bot_cfg.client_secret = *v;
            }
            if (auto v = (*bot)["agent"].value<std::string>()) {
                bot_cfg.agent = *v;
            }

            cfg.qq_bots.push_back(std::move(bot_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config_detail
