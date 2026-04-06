#include "config/config-detail.hpp"
#include "config/json-access.hpp"

#include <nlohmann/json.hpp>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::config::detail {

    Config parse_security_section(const nlohmann::json &root, Config cfg) {
        const auto *security = find_object_member(root, "security");
        if (security == nullptr) {
            return cfg;
        }

        if (const auto *array = find_array_member(*security, "allow"); array != nullptr) {
            for (const auto &item : *array) {
                if (item.is_string()) {
                    cfg.allow.push_back(item.get<std::string>());
                }
            }
        }

        if (const auto *array = find_array_member(*security, "deny"); array != nullptr) {
            for (const auto &item : *array) {
                if (item.is_string()) {
                    cfg.deny.push_back(item.get<std::string>());
                }
            }
        }

        return cfg;
    }

    Config parse_skills_section(const nlohmann::json &root, Config cfg) {
        const auto *skills = find_object_member(root, "skills");
        if (skills == nullptr) {
            return cfg;
        }

        if (const auto *array = find_array_member(*skills, "paths"); array != nullptr) {
            for (const auto &item : *array) {
                if (item.is_string()) {
                    cfg.skill_paths.push_back(expand_home_path(expand_env_vars(item.get<std::string>())));
                }
            }
        }

        return cfg;
    }

    Config parse_custom_tools_section(const nlohmann::json &root, Config cfg) {
        const auto *tools = find_object_member(root, "tools");
        if (tools == nullptr) {
            return cfg;
        }

        const auto *custom = find_array_member(*tools, "custom");
        if (custom == nullptr) {
            return cfg;
        }

        for (const auto &item : *custom) {
            if (!item.is_object()) {
                continue;
            }

            Config::ScriptToolConfig tool_cfg;
            if (const auto *value = find_member(item, "name"); value != nullptr && value->is_string()) {
                tool_cfg.name = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "description"); value != nullptr && value->is_string()) {
                tool_cfg.description = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "command"); value != nullptr && value->is_string()) {
                tool_cfg.command = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "timeout"); value != nullptr && value->is_number_integer()) {
                tool_cfg.timeout = value->get<int>();
            }
            if (const auto *value = find_member(item, "working_dir"); value != nullptr && value->is_string()) {
                tool_cfg.working_dir = expand_home_path(expand_env_vars(value->get<std::string>()));
            }

            if (const auto *schema = find_object_member(item, "input_schema"); schema != nullptr) {
                for (auto it = schema->begin(); it != schema->end(); ++it) {
                    if (it.value().is_string()) {
                        tool_cfg.input_schema.emplace(it.key(), it.value().get<std::string>());
                    }
                }
            }

            if (tool_cfg.name.empty()) {
                spdlog::warn("Custom tool missing 'name', skipping");
                continue;
            }
            if (tool_cfg.command.empty()) {
                spdlog::warn("Custom tool '{}' missing 'command', skipping", tool_cfg.name);
                continue;
            }

            cfg.custom_tools.push_back(std::move(tool_cfg));
        }

        return cfg;
    }

    Config parse_mcp_section(const nlohmann::json &root, Config cfg) {
        const auto *mcp = find_object_member(root, "mcp");
        if (mcp == nullptr) {
            return cfg;
        }

        const auto *servers = find_array_member(*mcp, "servers");
        if (servers == nullptr) {
            return cfg;
        }

        for (const auto &item : *servers) {
            if (!item.is_object()) {
                continue;
            }

            Config::McpServerConfig server_cfg;
            if (const auto *value = find_member(item, "name"); value != nullptr && value->is_string()) {
                server_cfg.name = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "command"); value != nullptr && value->is_string()) {
                server_cfg.command = expand_home_path(expand_env_vars(value->get<std::string>()));
            }
            if (const auto *array = find_array_member(item, "args"); array != nullptr) {
                for (const auto &arg_item : *array) {
                    if (arg_item.is_string()) {
                        server_cfg.args.push_back(expand_home_path(expand_env_vars(arg_item.get<std::string>())));
                    }
                }
            }
            if (const auto *env = find_object_member(item, "env"); env != nullptr) {
                for (auto it = env->begin(); it != env->end(); ++it) {
                    if (it.value().is_string()) {
                        server_cfg.env.emplace(it.key(), expand_env_vars(it.value().get<std::string>()));
                    }
                }
            }
            if (const auto *value = find_member(item, "timeout"); value != nullptr && value->is_number_integer()) {
                server_cfg.timeout = value->get<int>();
            }

            if (server_cfg.name.empty()) {
                spdlog::warn("MCP server missing 'name', skipping");
                continue;
            }
            if (server_cfg.command.empty()) {
                spdlog::warn("MCP server '{}' missing 'command', skipping", server_cfg.name);
                continue;
            }
            if (server_cfg.timeout <= 0) {
                spdlog::warn("MCP server '{}' has invalid timeout {}, defaulting to 30", server_cfg.name, server_cfg.timeout);
                server_cfg.timeout = 30;
            }

            auto existing = std::ranges::find(cfg.mcp_servers, server_cfg.name, &Config::McpServerConfig::name);
            if (existing != cfg.mcp_servers.end()) {
                spdlog::warn("Duplicate MCP server '{}' found; later entry overwrites earlier one", server_cfg.name);
                *existing = std::move(server_cfg);
                continue;
            }

            cfg.mcp_servers.push_back(std::move(server_cfg));
        }

        return cfg;
    }

    Config parse_hooks_section(const nlohmann::json &root, Config cfg) {
        const auto *hooks = find_object_member(root, "hooks");
        if (hooks == nullptr) {
            return cfg;
        }

        if (const auto *array = find_array_member(*hooks, "paths"); array != nullptr) {
            for (const auto &item : *array) {
                if (item.is_string()) {
                    cfg.hook_paths.push_back(expand_home_path(expand_env_vars(item.get<std::string>())));
                }
            }
        }

        return cfg;
    }

    Config parse_heartbeat_section(const nlohmann::json &root, Config cfg) {
        const auto *heartbeat = find_object_member(root, "heartbeat");
        if (heartbeat == nullptr) {
            return cfg;
        }

        if (const auto *value = find_member(*heartbeat, "heartbeat_md_path"); value != nullptr && value->is_string()) {
            cfg.heartbeat_md_path = expand_home_path(expand_env_vars(value->get<std::string>()));
        }
        if (const auto *value = find_member(*heartbeat, "ack_max_chars"); value != nullptr && value->is_number_integer()) {
            cfg.ack_max_chars = value->get<int>();
        }
        if (const auto *value = find_member(*heartbeat, "isolated_session"); value != nullptr && value->is_boolean()) {
            cfg.isolated_session = value->get<bool>();
        }
        if (const auto *value = find_member(*heartbeat, "light_context"); value != nullptr && value->is_boolean()) {
            cfg.light_context = value->get<bool>();
        }

        const auto *jobs = find_array_member(*heartbeat, "jobs");
        if (jobs == nullptr) {
            return cfg;
        }

        for (const auto &item : *jobs) {
            if (!item.is_object()) {
                continue;
            }

            Config::HeartbeatJobConfig job_cfg;
            if (const auto *value = find_member(item, "name"); value != nullptr && value->is_string()) {
                job_cfg.name = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "cron"); value != nullptr && value->is_string()) {
                job_cfg.cron = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "prompt"); value != nullptr && value->is_string()) {
                job_cfg.prompt = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "agent"); value != nullptr && value->is_string()) {
                job_cfg.agent = value->get<std::string>();
            }
            if (const auto *value = find_member(item, "channel"); value != nullptr && value->is_string()) {
                job_cfg.channel = value->get<std::string>();
            }

            if (job_cfg.name.empty() || job_cfg.cron.empty() || job_cfg.prompt.empty()) {
                spdlog::warn("Heartbeat job missing required fields (name, cron, prompt), skipping");
                continue;
            }

            cfg.heartbeat_jobs.push_back(std::move(job_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config::detail
