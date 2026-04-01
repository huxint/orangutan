#pragma once

#include "tools/registry/tool.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>

namespace orangutan::bootstrap {

    struct CliOptions {
        std::string api_key;
        std::string cli_model;
        std::string cli_base_url;
        std::string cli_provider;
        std::string cli_agent_key = "default";
        std::string message;
        std::string cli_system_prompt;
        std::string resume_session;
        bool dump_session = false;
        bool event_stream = false;
        bool cli_mode = false;
        bool channel_mode = false;
        std::string edit_mode;
        std::string config_password;
        std::string protect_config_path;
        bool verbose = false;
        bool resume_requested = false;
        bool protect_config_requested = false;
        bool web_mode = false;
        int web_port = 18080;
        std::string web_host = "127.0.0.1";
        std::string web_dir = "web/dist";
    };

    [[nodiscard]]
    std::string read_stdin_if_piped();

    void emit_json_event(const nlohmann::json &event);

    void configure_cli_app(CLI::App &app, CliOptions &options, CLI::Option *&resume_flag, CLI::Option *&protect_flag);

    void configure_logging(bool verbose);

    [[nodiscard]]
    ToolApprovalCallback make_cli_approval_callback(bool allow_prompting);

    [[nodiscard]]
    bool validate_initial_options(const CliOptions &options);

    int run_protect_config_mode(const CliOptions &options);

} // namespace orangutan::bootstrap
