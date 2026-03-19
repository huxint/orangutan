#pragma once

#include "core/tools/permissions.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan {

struct AgentConfig {
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-20250514";
    std::vector<std::string> fallback_models;
    std::string base_url = "https://api.anthropic.com";
    std::string api_key;
    std::string system_prompt;
    std::string workspace;
    ToolPermissionSettings permissions{
        .sandbox_mode = ToolSandboxMode::isolated,
        .shell_approval = ToolApprovalPolicy::ask,
    };
    std::vector<std::string> subagents;
    std::string edit_mode = "hashline";
};

struct QqBotConfig {
    std::string name;
    std::string app_id;
    std::string client_secret;
    std::string agent = "default";
};

struct Config {
    // [agent] section
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-20250514";
    std::vector<std::string> fallback_models;
    std::string base_url = "https://api.anthropic.com";
    std::string api_key;
    double temperature = 1.0;
    int max_iterations = 20;
    int max_tokens = 4096;
    std::string workspace;

    // [tools] section
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;
    std::string edit_mode = "hashline";  // "hashline" | "search_replace"

    // [permissions] section
    ToolPermissionSettings permissions{
        .sandbox_mode = ToolSandboxMode::isolated,
        .shell_approval = ToolApprovalPolicy::ask,
    };

    // [session] section
    bool auto_save = true;

    struct MemoryConfig {
        bool mirror_enabled = false;
        std::string mirror_file = "MEMORY.md";
        std::string journal_dir = "memory";
    };
    MemoryConfig memory;

    // [agent] system_prompt
    std::string system_prompt;
    // [qq] section
    std::string qq_app_id;
    std::string qq_client_secret;
    std::vector<QqBotConfig> qq_bots;

    // [agents.<key>] sections
    std::unordered_map<std::string, AgentConfig> agents;

    // [security] section
    std::vector<std::string> allow;
    std::vector<std::string> deny;

    // [skills] section
    std::vector<std::string> skill_paths;

    // [[tools.custom]] section
    struct ScriptToolConfig {
        std::string name;
        std::string description;
        std::string command;
        std::unordered_map<std::string, std::string> input_schema;
        int timeout = 30;
        std::string working_dir;
    };
    std::vector<ScriptToolConfig> custom_tools;

    // [[mcp.servers]] section
    struct McpServerConfig {
        std::string name;
        std::string command;
        std::vector<std::string> args;
        std::unordered_map<std::string, std::string> env;
        int timeout = 30;
    };
    std::vector<McpServerConfig> mcp_servers;

    // [hooks] section
    std::vector<std::string> hook_paths;

    // [heartbeat] section
    std::string heartbeat_md_path;
    int ack_max_chars = 300;
    bool isolated_session = false;
    bool light_context = false;

    // [[heartbeat.jobs]] section
    struct HeartbeatJobConfig {
        std::string name;
        std::string cron;
        std::string prompt;
        std::string agent = "default";
        std::string channel = "cli";
    };
    std::vector<HeartbeatJobConfig> heartbeat_jobs;

    // Load config from ~/.orangutan/config.toml (if it exists)
    // Returns a Config with defaults if file is missing
    static Config load();

    // Load config from a specific path (for testing)
    static Config load_from(const std::string &path);

    // Check if a tool is permitted by the allow/deny lists
    [[nodiscard]]
    bool is_tool_allowed(const std::string &name) const;

    [[nodiscard]]
    std::optional<AgentConfig> find_agent(const std::string &key) const;
};

// Expand ${VAR_NAME} patterns in a string with environment variable values.
// Undefined variables are replaced with empty string (with a warning logged).
std::string expand_env_vars(const std::string &input);

// Expand "~" or "~/" to the current user's home directory.
std::string expand_home_path(const std::string &input);

} // namespace orangutan
