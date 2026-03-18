#include "infra/config/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

namespace orangutan {

std::string expand_env_vars(const std::string &input) {
    std::string result;
    result.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        // Look for "${" pattern
        auto start = input.find("${", pos);
        if (start == std::string::npos) {
            result.append(input, pos);
            break;
        }

        // Append everything before the "${"
        result.append(input, pos, start - pos);

        auto end = input.find('}', start + 2);
        if (end == std::string::npos) {
            // No closing brace — keep literal text
            result.append(input, start);
            break;
        }

        auto var_name = input.substr(start + 2, end - start - 2);
        const char *val = std::getenv(var_name.c_str());
        if (val != nullptr) {
            result.append(val);
        } else {
            spdlog::warn("Environment variable '{}' is not set, replacing with empty string", var_name);
        }

        pos = end + 1;
    }

    return result;
}

std::string expand_home_path(const std::string &input) {
    if (input.empty() || input[0] != '~') {
        return input;
    }

    if (input.size() > 1 && input[1] != '/') {
        spdlog::warn("Path '{}' uses unsupported ~user syntax; leaving it unchanged", input);
        return input;
    }

    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        spdlog::warn("HOME is not set; cannot expand path '{}'", input);
        return input;
    }

    if (input == "~") {
        return home;
    }

    return std::string(home) + input.substr(1);
}

static std::string default_config_path() {
    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::string(home) + "/.orangutan/config.toml";
}

static AgentConfig make_agent_config_from_legacy(const Config &cfg) {
    return {
        .provider = cfg.provider,
        .model = cfg.model,
        .base_url = cfg.base_url,
        .api_key = cfg.api_key,
        .system_prompt = cfg.system_prompt,
        .workspace = cfg.workspace,
        .permissions = cfg.permissions,
        .subagents = {},
    };
}

static void expand_agent_config(AgentConfig &cfg) {
    cfg.api_key = expand_env_vars(cfg.api_key);
    cfg.base_url = expand_env_vars(cfg.base_url);
    cfg.model = expand_env_vars(cfg.model);
    cfg.provider = expand_env_vars(cfg.provider);
    cfg.system_prompt = expand_env_vars(cfg.system_prompt);
    cfg.workspace = expand_home_path(expand_env_vars(cfg.workspace));

    for (auto &subagent : cfg.subagents) {
        subagent = expand_env_vars(subagent);
    }
}

static Config parse_agent_section(const toml::table &tbl, Config cfg) {
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
    if (auto v = (*agent)["base_url"].value<std::string>()) {
        cfg.base_url = *v;
    }
    if (auto v = (*agent)["temperature"].value<double>()) {
        cfg.temperature = *v;
    }
    if (auto v = (*agent)["max_iterations"].value<int64_t>()) {
        cfg.max_iterations = static_cast<int>(*v);
    }
    if (auto v = (*agent)["max_tokens"].value<int64_t>()) {
        cfg.max_tokens = static_cast<int>(*v);
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

static Config parse_tools_section(const toml::table &tbl, Config cfg) {
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

    return cfg;
}

static void apply_permissions_table(const toml::table &permissions, ToolPermissionSettings &settings) {
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

static Config parse_permissions_section(const toml::table &tbl, Config cfg) {
    const auto *permissions = tbl["permissions"].as_table();
    if (permissions == nullptr) {
        return cfg;
    }

    apply_permissions_table(*permissions, cfg.permissions);

    return cfg;
}

static Config parse_session_section(const toml::table &tbl, Config cfg) {
    const auto *session = tbl["session"].as_table();
    if (session == nullptr) {
        return cfg;
    }

    if (auto v = (*session)["auto_save"].value<bool>()) {
        cfg.auto_save = *v;
    }

    return cfg;
}

static Config parse_memory_section(const toml::table &tbl, Config cfg) {
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

static Config parse_qq_section(const toml::table &tbl, Config cfg) {
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

static Config parse_agents_section(const toml::table &tbl, Config cfg) {
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

        cfg.agents.insert_or_assign(key, std::move(agent_cfg));
    }

    return cfg;
}

static Config parse_qq_bots_section(const toml::table &tbl, Config cfg) {
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

static Config parse_security_section(const toml::table &tbl, Config cfg) {
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

static Config parse_skills_section(const toml::table &tbl, Config cfg) {
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

static Config parse_custom_tools_section(const toml::table &tbl, Config cfg) {
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

        // Parse input_schema as a flat table of param_name → type_string
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

static Config parse_mcp_section(const toml::table &tbl, Config cfg) {
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

static Config parse_hooks_section(const toml::table &tbl, Config cfg) {
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

static Config parse_heartbeat_section(const toml::table &tbl, Config cfg) {
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

static Config parse_toml(const toml::table &tbl) {
    Config cfg;
    cfg = parse_agent_section(tbl, std::move(cfg));
    cfg = parse_tools_section(tbl, std::move(cfg));
    cfg = parse_permissions_section(tbl, std::move(cfg));
    cfg = parse_session_section(tbl, std::move(cfg));
    cfg = parse_memory_section(tbl, std::move(cfg));
    cfg = parse_qq_section(tbl, std::move(cfg));
    cfg = parse_agents_section(tbl, std::move(cfg));
    cfg = parse_qq_bots_section(tbl, std::move(cfg));
    cfg = parse_security_section(tbl, std::move(cfg));
    cfg = parse_skills_section(tbl, std::move(cfg));
    cfg = parse_custom_tools_section(tbl, std::move(cfg));
    cfg = parse_mcp_section(tbl, std::move(cfg));
    cfg = parse_hooks_section(tbl, std::move(cfg));
    cfg = parse_heartbeat_section(tbl, std::move(cfg));

    // Expand environment variables in all string config values
    cfg.api_key = expand_env_vars(cfg.api_key);
    cfg.base_url = expand_env_vars(cfg.base_url);
    cfg.model = expand_env_vars(cfg.model);
    cfg.provider = expand_env_vars(cfg.provider);
    cfg.system_prompt = expand_env_vars(cfg.system_prompt);
    cfg.workspace = expand_home_path(expand_env_vars(cfg.workspace));
    cfg.memory.mirror_file = expand_home_path(expand_env_vars(cfg.memory.mirror_file));
    cfg.memory.journal_dir = expand_home_path(expand_env_vars(cfg.memory.journal_dir));
    cfg.qq_app_id = expand_env_vars(cfg.qq_app_id);
    cfg.qq_client_secret = expand_env_vars(cfg.qq_client_secret);

    if (cfg.agents.empty() || !cfg.agents.contains("default")) {
        cfg.agents.insert_or_assign("default", make_agent_config_from_legacy(cfg));
    }

    for (auto &[key, agent_cfg] : cfg.agents) {
        expand_agent_config(agent_cfg);
        (void)key;
    }

    if (cfg.qq_bots.empty() && (!cfg.qq_app_id.empty() || !cfg.qq_client_secret.empty())) {
        cfg.qq_bots.push_back({
            .name = {},
            .app_id = cfg.qq_app_id,
            .client_secret = cfg.qq_client_secret,
            .agent = "default",
        });
    }

    for (auto &bot : cfg.qq_bots) {
        bot.name = expand_env_vars(bot.name);
        bot.app_id = expand_env_vars(bot.app_id);
        bot.client_secret = expand_env_vars(bot.client_secret);
        bot.agent = expand_env_vars(bot.agent);

        if (bot.agent.empty()) {
            bot.agent = "default";
        }
        if (!cfg.agents.contains(bot.agent)) {
            throw std::runtime_error("QQ bot references unknown agent: " + bot.agent);
        }
    }

    return cfg;
}

Config Config::load() {
    auto path = default_config_path();
    if (path.empty() || !std::filesystem::exists(path)) {
        return {};
    }
    return load_from(path);
}

Config Config::load_from(const std::string &path) {
    try {
        auto tbl = toml::parse_file(path);
        return parse_toml(tbl);
    } catch (const toml::parse_error &e) {
        spdlog::warn("Failed to parse config file {}: {}", path, e.what());
        return {};
    } catch (const std::runtime_error &e) {
        spdlog::warn("Invalid config file {}: {}", path, e.what());
        return {};
    }
}

bool Config::is_tool_allowed(const std::string &name) const {
    auto effective = permissions;
    for (const auto &tool : allowed_tools) {
        if (std::ranges::find(effective.allowed_tools, tool) == effective.allowed_tools.end()) {
            effective.allowed_tools.push_back(tool);
        }
    }
    for (const auto &tool : denied_tools) {
        if (std::ranges::find(effective.denied_tools, tool) == effective.denied_tools.end()) {
            effective.denied_tools.push_back(tool);
        }
    }
    return orangutan::is_tool_allowed(effective, name);
}

std::optional<AgentConfig> Config::find_agent(const std::string &key) const {
    if (const auto it = agents.find(key); it != agents.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace orangutan
