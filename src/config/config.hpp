#pragma once

#include "types/types.hpp"
#include "config/secret-protection.hpp"
#include "permissions/permission-state.hpp"
#include "utils/transparent-lookup.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::config {

    struct ModelCostConfig {
        double input = 0.0;
        double output = 0.0;
    };

    struct ModelConfig {
        std::string provider;
        std::string protocol;
        std::optional<int> max_tokens;
        std::optional<int> context_window;
        std::string thinking = "none";
        std::optional<ModelCostConfig> cost;
    };

    struct ProfileConfig {
        std::string base_url;
        std::string api_key;
        utils::transparent_string_unordered_map<std::string> headers;
        std::unordered_map<std::string, ModelConfig> models;
    };

    struct FallbackModelRef {
        std::string profile;
        std::string model;

        FallbackModelRef() = default;
        FallbackModelRef(const char *model_name)
        : model(model_name != nullptr ? model_name : "") {}
        FallbackModelRef(std::string model_name)
        : model(std::move(model_name)) {}
        FallbackModelRef(std::string profile_name, std::string model_name)
        : profile(std::move(profile_name)),
          model(std::move(model_name)) {}
    };

    struct AgentConfig {
        std::string profile;
        std::string model = "claude-sonnet-4-20250514";
        std::vector<FallbackModelRef> fallback_models;
        std::string workspace;
        PermissionConfig permissions_config;
        bool leader_mode = false;
        int max_concurrent_agents = 4;
        int thinking_budget = 0;
    };

    struct QqBotConfig {
        std::string name;
        std::string app_id;
        std::string client_secret;
        std::string agent = "default";
    };

    struct Config {
        // agent defaults object
        std::string profile;
        std::string model = "claude-sonnet-4-20250514";
        std::vector<FallbackModelRef> fallback_models;
        double temperature = 1.0;
        int max_iterations = 20;
        int max_tokens = 4096;
        int thinking_budget = 0;
        std::string workspace;

        // permissions object
        PermissionConfig permissions_config;

        // session object
        bool auto_save = true;

        struct MemoryConfig {
            bool mirror_enabled = false;
            std::string mirror_file = ".orangutan/memory/MEMORY.md";
            std::string journal_dir = ".orangutan/memory/journal";
        };
        MemoryConfig memory;

        // qq object
        std::string qq_app_id;
        std::string qq_client_secret;
        std::vector<QqBotConfig> qq_bots;

        // profiles object
        std::unordered_map<std::string, ProfileConfig> profiles;

        // agents object
        std::unordered_map<std::string, AgentConfig> agents;

        // security object
        std::vector<std::string> allow;
        std::vector<std::string> deny;

        // skills object
        std::vector<std::string> skill_paths;

        // tools.custom array
        struct ScriptToolConfig {
            std::string name;
            std::string description;
            std::string command;
            std::unordered_map<std::string, std::string> input_schema;
            int timeout = 30;
            std::string working_dir;
        };
        std::vector<ScriptToolConfig> custom_tools;

        // mcp.servers array
        struct McpServerConfig {
            std::string name;
            std::string command;
            std::vector<std::string> args;
            std::unordered_map<std::string, std::string> env;
            int timeout = 30;
        };
        std::vector<McpServerConfig> mcp_servers;

        // hooks object
        std::vector<std::string> hook_paths;

        // Load config from ~/.orangutan/config.json (if it exists)
        // Returns a Config with defaults if file is missing
        static Config load(const ConfigSecretOptions &secret_options = {});

        // Load config from a specific path (for testing)
        static Config load_from(const std::filesystem::path &path, const ConfigSecretOptions &secret_options = {});

        [[nodiscard]]
        std::optional<AgentConfig> find_agent(const std::string &key) const;

        // Serialize config to JSON and write to file
        void save_to(const std::filesystem::path &path) const;
    };

    // Expand ${VAR_NAME} patterns in a string with environment variable values.
    // Undefined variables are replaced with empty string (with a warning logged).
    std::string expand_env_vars(const std::string &input);

} // namespace orangutan::config

namespace orangutan {

    using config::AgentConfig;
    using config::Config;
    using config::expand_env_vars;
    using config::FallbackModelRef;
    using config::ModelConfig;
    using config::ModelCostConfig;
    using config::ProfileConfig;
    using config::QqBotConfig;

} // namespace orangutan
