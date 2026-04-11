#include "config/config-detail.hpp"
#include "config/json-access.hpp"

#include <array>
#include <utility>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

namespace orangutan::config::detail {

    namespace {

        using script_tool_string_field = string_member_field<Config::ScriptToolConfig>;
        constexpr auto SCRIPT_TOOL_STRING_FIELDS = std::to_array<script_tool_string_field>({
            {"name", &Config::ScriptToolConfig::name},
            {"description", &Config::ScriptToolConfig::description},
            {"command", &Config::ScriptToolConfig::command},
        });

        using heartbeat_job_string_field = string_member_field<Config::HeartbeatJobConfig>;
        constexpr auto HEARTBEAT_JOB_STRING_FIELDS = std::to_array<heartbeat_job_string_field>({
            {"name", &Config::HeartbeatJobConfig::name},
            {"cron", &Config::HeartbeatJobConfig::cron},
            {"prompt", &Config::HeartbeatJobConfig::prompt},
            {"agent", &Config::HeartbeatJobConfig::agent},
            {"channel", &Config::HeartbeatJobConfig::channel},
        });

    } // namespace

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
            assign_string_members(item, tool_cfg, SCRIPT_TOOL_STRING_FIELDS);
            static_cast<void>(assign_number_member(item, "timeout", tool_cfg, &Config::ScriptToolConfig::timeout));
            if (assign_string_member(item, "working_dir", tool_cfg, &Config::ScriptToolConfig::working_dir)) {
                tool_cfg.working_dir = expand_home_path(expand_env_vars(tool_cfg.working_dir));
            }

            if (const auto *schema = find_object_member(item, "input_schema"); schema != nullptr) {
                for (auto it = schema->begin(); it != schema->end(); ++it) {
                    if (it.value().is_string()) {
                        tool_cfg.input_schema.try_emplace(it.key(), it.value().get<std::string>());
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
            static_cast<void>(assign_string_member(item, "name", server_cfg, &Config::McpServerConfig::name));
            if (assign_string_member(item, "command", server_cfg, &Config::McpServerConfig::command)) {
                server_cfg.command = expand_home_path(expand_env_vars(server_cfg.command));
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
                        server_cfg.env.try_emplace(it.key(), expand_env_vars(it.value().get<std::string>()));
                    }
                }
            }
            static_cast<void>(assign_number_member(item, "timeout", server_cfg, &Config::McpServerConfig::timeout));

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

        if (assign_string_member(*heartbeat, "heartbeat_md_path", cfg, &Config::heartbeat_md_path)) {
            cfg.heartbeat_md_path = expand_home_path(expand_env_vars(cfg.heartbeat_md_path));
        }
        static_cast<void>(assign_number_member(*heartbeat, "ack_max_chars", cfg, &Config::ack_max_chars));
        static_cast<void>(assign_bool_member(*heartbeat, "isolated_session", cfg, &Config::isolated_session));
        static_cast<void>(assign_bool_member(*heartbeat, "light_context", cfg, &Config::light_context));

        const auto *jobs = find_array_member(*heartbeat, "jobs");
        if (jobs == nullptr) {
            return cfg;
        }

        for (const auto &item : *jobs) {
            if (!item.is_object()) {
                continue;
            }

            Config::HeartbeatJobConfig job_cfg;
            assign_string_members(item, job_cfg, HEARTBEAT_JOB_STRING_FIELDS);

            if (job_cfg.name.empty() || job_cfg.cron.empty() || job_cfg.prompt.empty()) {
                spdlog::warn("Heartbeat job missing required fields (name, cron, prompt), skipping");
                continue;
            }

            cfg.heartbeat_jobs.push_back(std::move(job_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config::detail
