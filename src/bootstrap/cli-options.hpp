#pragma once

#include "permissions/permission-state.hpp"
#include "tools/registry/tool.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>

namespace orangutan::bootstrap {

    struct CliOptions {
        std::string api_key;
        std::string cli_model;
        std::string cli_agent_key = "default";
        std::string message;
        std::string resume_session;
        bool dump_session = false;
        bool event_stream = false;
        bool cli_mode = false;
        bool channel_mode = false;
        std::string config_password;
        std::string protect_config_path;
        bool verbose = false;
        bool resume_requested = false;
        bool protect_config_requested = false;
        bool web_mode = false;
        int web_port = 18080;
        std::string web_host = "127.0.0.1";
        std::string web_dir = "web/dist";
        std::string permission_mode_str;
        bool dangerously_skip_permissions = false;
        std::string allowed_tools_str;
        std::string disallowed_tools_str;
    };

    [[nodiscard]]
    std::string read_stdin_if_piped();

    void emit_json_event(const nlohmann::json &event);

    void configure_cli_app(CLI::App &app, CliOptions &options, CLI::Option *&resume_flag, CLI::Option *&protect_flag);

    void configure_logging(bool verbose);

    [[nodiscard]]
    std::string format_cli_permission_prompt(const ToolUse &call, const PermissionDecision &decision);

    [[nodiscard]]
    ApprovalCallback make_cli_approval_callback(bool allow_prompting);

    [[nodiscard]]
    bool validate_initial_options(const CliOptions &options);

    int run_protect_config_mode(const CliOptions &options);

    [[nodiscard]]
    CLIPermissionOptions build_cli_permission_options(const CliOptions &options);

} // namespace orangutan::bootstrap
