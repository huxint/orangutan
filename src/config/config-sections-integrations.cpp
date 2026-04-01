#include "config/config-detail.hpp"

#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::config_detail {

    Config parse_security_section(const toml::table &tbl, Config cfg) {
        const auto *security = tbl["security"].as_table();
        if (security == nullptr) {
            return cfg;
        }

        if (const auto *arr = (*security)["allow"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.allow.push_back(*s);
                }
            }
        }

        if (const auto *arr = (*security)["deny"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.deny.push_back(*s);
                }
            }
        }

        return cfg;
    }

    Config parse_skills_section(const toml::table &tbl, Config cfg) {
        const auto *skills = tbl["skills"].as_table();
        if (skills == nullptr) {
            return cfg;
        }

        if (const auto *arr = (*skills)["paths"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.skill_paths.push_back(expand_home_path(expand_env_vars(*s)));
                }
            }
        }

        return cfg;
    }

    Config parse_custom_tools_section(const toml::table &tbl, Config cfg) {
        const auto *tools = tbl["tools"].as_table();
        if (tools == nullptr) {
            return cfg;
        }

        const auto *custom = (*tools)["custom"].as_array();
        if (custom == nullptr) {
            return cfg;
        }

        for (const auto &item : *custom) {
            const auto *tool = item.as_table();
            if (tool == nullptr) {
                continue;
            }

            Config::ScriptToolConfig tool_cfg;
            if (auto v = (*tool)["name"].value<std::string>()) {
                tool_cfg.name = *v;
            }
            if (auto v = (*tool)["description"].value<std::string>()) {
                tool_cfg.description = *v;
            }
            if (auto v = (*tool)["command"].value<std::string>()) {
                // Keep ${param} placeholders intact for script-tool substitution at execution time.
                tool_cfg.command = *v;
            }
            if (auto v = (*tool)["timeout"].value<int64_t>()) {
                tool_cfg.timeout = static_cast<int>(*v);
            }
            if (auto v = (*tool)["working_dir"].value<std::string>()) {
                tool_cfg.working_dir = expand_home_path(expand_env_vars(*v));
            }

            if (const auto *schema = (*tool)["input_schema"].as_table()) {
                for (const auto &[key_node, value_node] : *schema) {
                    if (auto s = value_node.value<std::string>()) {
                        tool_cfg.input_schema.emplace(std::string(key_node.str()), *s);
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

    Config parse_mcp_section(const toml::table &tbl, Config cfg) {
        const auto *mcp = tbl["mcp"].as_table();
        if (mcp == nullptr) {
            return cfg;
        }

        const auto *servers = (*mcp)["servers"].as_array();
        if (servers == nullptr) {
            return cfg;
        }

        for (const auto &item : *servers) {
            const auto *server = item.as_table();
            if (server == nullptr) {
                continue;
            }

            Config::McpServerConfig server_cfg;
            if (auto v = (*server)["name"].value<std::string>()) {
                server_cfg.name = *v;
            }
            if (auto v = (*server)["command"].value<std::string>()) {
                server_cfg.command = expand_home_path(expand_env_vars(*v));
            }
            if (const auto *arr = (*server)["args"].as_array()) {
                for (const auto &arg_item : *arr) {
                    if (auto s = arg_item.value<std::string>()) {
                        server_cfg.args.push_back(expand_home_path(expand_env_vars(*s)));
                    }
                }
            }
            if (const auto *env = (*server)["env"].as_table()) {
                for (const auto &[key_node, value_node] : *env) {
                    if (auto s = value_node.value<std::string>()) {
                        server_cfg.env.emplace(std::string(key_node.str()), expand_env_vars(*s));
                    }
                }
            }
            if (auto v = (*server)["timeout"].value<int64_t>()) {
                server_cfg.timeout = static_cast<int>(*v);
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

    Config parse_hooks_section(const toml::table &tbl, Config cfg) {
        const auto *hooks = tbl["hooks"].as_table();
        if (hooks == nullptr) {
            return cfg;
        }

        if (const auto *arr = (*hooks)["paths"].as_array()) {
            for (const auto &item : *arr) {
                if (auto s = item.value<std::string>()) {
                    cfg.hook_paths.push_back(expand_home_path(expand_env_vars(*s)));
                }
            }
        }

        return cfg;
    }

    Config parse_heartbeat_section(const toml::table &tbl, Config cfg) {
        const auto *heartbeat = tbl["heartbeat"].as_table();
        if (heartbeat == nullptr) {
            return cfg;
        }

        if (auto v = (*heartbeat)["heartbeat_md_path"].value<std::string>()) {
            cfg.heartbeat_md_path = expand_home_path(expand_env_vars(*v));
        }
        if (auto v = (*heartbeat)["ack_max_chars"].value<int64_t>()) {
            cfg.ack_max_chars = static_cast<int>(*v);
        }
        if (auto v = (*heartbeat)["isolated_session"].value<bool>()) {
            cfg.isolated_session = *v;
        }
        if (auto v = (*heartbeat)["light_context"].value<bool>()) {
            cfg.light_context = *v;
        }

        const auto *jobs = (*heartbeat)["jobs"].as_array();
        if (jobs == nullptr) {
            return cfg;
        }

        for (const auto &item : *jobs) {
            const auto *job = item.as_table();
            if (job == nullptr) {
                continue;
            }

            Config::HeartbeatJobConfig job_cfg;
            if (auto v = (*job)["name"].value<std::string>()) {
                job_cfg.name = *v;
            }
            if (auto v = (*job)["cron"].value<std::string>()) {
                job_cfg.cron = *v;
            }
            if (auto v = (*job)["prompt"].value<std::string>()) {
                job_cfg.prompt = *v;
            }
            if (auto v = (*job)["agent"].value<std::string>()) {
                job_cfg.agent = *v;
            }
            if (auto v = (*job)["channel"].value<std::string>()) {
                job_cfg.channel = *v;
            }

            if (job_cfg.name.empty() || job_cfg.cron.empty() || job_cfg.prompt.empty()) {
                spdlog::warn("Heartbeat job missing required fields (name, cron, prompt), skipping");
                continue;
            }

            cfg.heartbeat_jobs.push_back(std::move(job_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config_detail
