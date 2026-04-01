#include "config/config-detail.hpp"

#include <nlohmann/json.hpp>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::config::detail {
    namespace {

        const nlohmann::json *find_member(const nlohmann::json &object, std::string_view key) {
            if (!object.is_object()) {
                return nullptr;
            }
            const auto it = object.find(std::string(key));
            return it == object.end() ? nullptr : &*it;
        }

        const nlohmann::json *find_object_member(const nlohmann::json &object, std::string_view key) {
            const auto *value = find_member(object, key);
            return value != nullptr && value->is_object() ? value : nullptr;
        }

        const nlohmann::json *find_array_member(const nlohmann::json &object, std::string_view key) {
            const auto *value = find_member(object, key);
            return value != nullptr && value->is_array() ? value : nullptr;
        }

        void assign_string_array(const nlohmann::json &array, std::vector<std::string> &target) {
            target.clear();
            if (!array.is_array()) {
                return;
            }

            for (const auto &item : array) {
                if (item.is_string()) {
                    target.push_back(item.get<std::string>());
                }
            }
        }

        void assign_expanded_string_array(const nlohmann::json &array, std::vector<std::string> &target) {
            target.clear();
            if (!array.is_array()) {
                return;
            }

            for (const auto &item : array) {
                if (item.is_string()) {
                    target.push_back(expand_env_vars(item.get<std::string>()));
                }
            }
        }

    } // namespace

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

    Config parse_agent_section(const nlohmann::json &root, Config cfg) {
        const auto *agent = find_object_member(root, "agent");
        if (agent == nullptr) {
            return cfg;
        }

        if (const auto *value = find_member(*agent, "provider"); value != nullptr && value->is_string()) {
            cfg.provider = value->get<std::string>();
        }
        if (const auto *value = find_member(*agent, "model"); value != nullptr && value->is_string()) {
            cfg.model = value->get<std::string>();
        }
        if (const auto *value = find_array_member(*agent, "fallback_models"); value != nullptr) {
            assign_string_array(*value, cfg.fallback_models);
        }
        if (const auto *value = find_member(*agent, "base_url"); value != nullptr && value->is_string()) {
            cfg.base_url = value->get<std::string>();
        }
        if (const auto *value = find_member(*agent, "temperature"); value != nullptr && value->is_number()) {
            cfg.temperature = value->get<base::f64>();
        }
        if (const auto *value = find_member(*agent, "max_iterations"); value != nullptr && value->is_number_integer()) {
            cfg.max_iterations = value->get<int>();
        }
        if (const auto *value = find_member(*agent, "max_tokens"); value != nullptr && value->is_number_integer()) {
            cfg.max_tokens = value->get<int>();
        }
        if (const auto *value = find_member(*agent, "thinking_budget"); value != nullptr && value->is_number_integer()) {
            cfg.thinking_budget = value->get<int>();
        }
        if (const auto *value = find_member(*agent, "workspace"); value != nullptr && value->is_string()) {
            cfg.workspace = value->get<std::string>();
        }
        if (const auto *value = find_member(*agent, "system_prompt"); value != nullptr && value->is_string()) {
            cfg.system_prompt = value->get<std::string>();
        }
        if (const auto *value = find_member(*agent, "api_key"); value != nullptr && value->is_string()) {
            cfg.api_key = value->get<std::string>();
        }

        return cfg;
    }

    Config parse_tools_section(const nlohmann::json &root, Config cfg) {
        const auto *tools = find_object_member(root, "tools");
        if (tools == nullptr) {
            return cfg;
        }

        if (const auto *array = find_array_member(*tools, "allowed"); array != nullptr) {
            cfg.allowed_tools.clear();
            cfg.permissions.allowed_tools.clear();
            for (const auto &item : *array) {
                if (item.is_string()) {
                    auto value = item.get<std::string>();
                    cfg.allowed_tools.push_back(value);
                    cfg.permissions.allowed_tools.push_back(std::move(value));
                }
            }
        }
        if (const auto *array = find_array_member(*tools, "denied"); array != nullptr) {
            cfg.denied_tools.clear();
            cfg.permissions.denied_tools.clear();
            for (const auto &item : *array) {
                if (item.is_string()) {
                    auto value = item.get<std::string>();
                    cfg.denied_tools.push_back(value);
                    cfg.permissions.denied_tools.push_back(std::move(value));
                }
            }
        }

        if (const auto *value = find_member(*tools, "edit_mode"); value != nullptr && value->is_string()) {
            cfg.edit_mode = value->get<std::string>();
        }

        return cfg;
    }

    void apply_permissions_object(const nlohmann::json &permissions, ToolPermissionSettings &settings) {
        if (const auto *value = find_member(permissions, "sandbox_mode"); value != nullptr && value->is_string()) {
            const auto expanded = expand_env_vars(value->get<std::string>());
            if (const auto parsed = parse_tool_sandbox_mode(expanded); parsed.has_value()) {
                settings.sandbox_mode = *parsed;
            } else {
                spdlog::warn("Unknown permissions.sandbox_mode '{}', keeping default '{}'", expanded, to_string(settings.sandbox_mode));
            }
        }

        if (const auto *value = find_member(permissions, "shell_approval_policy"); value != nullptr && value->is_string()) {
            const auto expanded = expand_env_vars(value->get<std::string>());
            if (const auto parsed = parse_tool_approval_policy(expanded); parsed.has_value()) {
                settings.shell_approval = *parsed;
            } else {
                spdlog::warn("Unknown permissions.shell_approval_policy '{}', keeping default '{}'", expanded, to_string(settings.shell_approval));
            }
        }

        if (const auto *array = find_array_member(permissions, "allowed_tools"); array != nullptr) {
            assign_expanded_string_array(*array, settings.allowed_tools);
        }
        if (const auto *array = find_array_member(permissions, "denied_tools"); array != nullptr) {
            assign_expanded_string_array(*array, settings.denied_tools);
        }
        if (const auto *array = find_array_member(permissions, "denied_shell_commands"); array != nullptr) {
            assign_expanded_string_array(*array, settings.denied_shell_commands);
        }
    }

    Config parse_permissions_section(const nlohmann::json &root, Config cfg) {
        const auto *permissions = find_object_member(root, "permissions");
        if (permissions == nullptr) {
            return cfg;
        }

        apply_permissions_object(*permissions, cfg.permissions);
        return cfg;
    }

    Config parse_session_section(const nlohmann::json &root, Config cfg) {
        const auto *session = find_object_member(root, "session");
        if (session == nullptr) {
            return cfg;
        }

        if (const auto *value = find_member(*session, "auto_save"); value != nullptr && value->is_boolean()) {
            cfg.auto_save = value->get<bool>();
        }

        return cfg;
    }

    Config parse_memory_section(const nlohmann::json &root, Config cfg) {
        const auto *memory = find_object_member(root, "memory");
        if (memory == nullptr) {
            return cfg;
        }

        if (const auto *value = find_member(*memory, "mirror_enabled"); value != nullptr && value->is_boolean()) {
            cfg.memory.mirror_enabled = value->get<bool>();
        }
        if (const auto *value = find_member(*memory, "mirror_file"); value != nullptr && value->is_string()) {
            cfg.memory.mirror_file = value->get<std::string>();
        }
        if (const auto *value = find_member(*memory, "journal_dir"); value != nullptr && value->is_string()) {
            cfg.memory.journal_dir = value->get<std::string>();
        }

        return cfg;
    }

    Config parse_qq_section(const nlohmann::json &root, Config cfg) {
        const auto *qq = find_object_member(root, "qq");
        if (qq == nullptr) {
            return cfg;
        }

        if (const auto *value = find_member(*qq, "app_id"); value != nullptr && value->is_string()) {
            cfg.qq_app_id = value->get<std::string>();
        }
        if (const auto *value = find_member(*qq, "client_secret"); value != nullptr && value->is_string()) {
            cfg.qq_client_secret = value->get<std::string>();
        }

        return cfg;
    }

    Config parse_agents_section(const nlohmann::json &root, Config cfg) {
        const auto *agents = find_object_member(root, "agents");
        if (agents == nullptr) {
            return cfg;
        }

        const auto legacy_defaults = make_agent_config_from_legacy(cfg);
        for (auto it = agents->begin(); it != agents->end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }

            auto agent_cfg = legacy_defaults;
            const auto &agent = it.value();

            if (const auto *value = find_member(agent, "provider"); value != nullptr && value->is_string()) {
                agent_cfg.provider = value->get<std::string>();
            }
            if (const auto *value = find_member(agent, "model"); value != nullptr && value->is_string()) {
                agent_cfg.model = value->get<std::string>();
            }
            if (const auto *value = find_array_member(agent, "fallback_models"); value != nullptr) {
                assign_string_array(*value, agent_cfg.fallback_models);
            }
            if (const auto *value = find_member(agent, "base_url"); value != nullptr && value->is_string()) {
                agent_cfg.base_url = value->get<std::string>();
            }
            if (const auto *value = find_member(agent, "api_key"); value != nullptr && value->is_string()) {
                agent_cfg.api_key = value->get<std::string>();
            }
            if (const auto *value = find_member(agent, "system_prompt"); value != nullptr && value->is_string()) {
                agent_cfg.system_prompt = value->get<std::string>();
            }
            if (const auto *value = find_member(agent, "workspace"); value != nullptr && value->is_string()) {
                agent_cfg.workspace = value->get<std::string>();
            }
            if (const auto *value = find_object_member(agent, "permissions"); value != nullptr) {
                apply_permissions_object(*value, agent_cfg.permissions);
            }
            if (const auto *value = find_array_member(agent, "subagents"); value != nullptr) {
                assign_string_array(*value, agent_cfg.subagents);
            }
            if (const auto *value = find_member(agent, "edit_mode"); value != nullptr && value->is_string()) {
                agent_cfg.edit_mode = value->get<std::string>();
            }
            if (const auto *value = find_member(agent, "thinking_budget"); value != nullptr && value->is_number_integer()) {
                agent_cfg.thinking_budget = value->get<int>();
            }

            cfg.agents.insert_or_assign(it.key(), std::move(agent_cfg));
        }

        return cfg;
    }

    Config parse_qq_bots_section(const nlohmann::json &root, Config cfg) {
        const auto *bots = find_array_member(root, "qq_bots");
        if (bots == nullptr) {
            return cfg;
        }

        for (const auto &item : *bots) {
            if (!item.is_object()) {
                continue;
            }

            QqBotConfig bot_cfg;
            if (const auto *value = find_member(item, "name"); value != nullptr && value->is_string()) {
                bot_cfg.name = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "app_id"); value != nullptr && value->is_string()) {
                bot_cfg.app_id = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "client_secret"); value != nullptr && value->is_string()) {
                bot_cfg.client_secret = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "agent"); value != nullptr && value->is_string()) {
                bot_cfg.agent = value->get<std::string>();
            }

            cfg.qq_bots.push_back(std::move(bot_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config::detail
