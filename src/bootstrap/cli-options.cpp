#include "bootstrap/cli-options.hpp"

#include "config/config.hpp"
#include "config/secret-protection.hpp"
#include "permissions/permission-display.hpp"
#include "permissions/permission-types.hpp"
#include "utils/string.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace orangutan::bootstrap {

    std::string read_stdin_if_piped() {
        if (isatty(STDIN_FILENO) != 0) {
            return {};
        }

        std::string content;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!content.empty()) {
                content += '\n';
            }
            content += line;
        }
        std::cin.clear();
        return content;
    }

    void emit_json_event(const nlohmann::json &event) {
        fmt::println("{}", event.dump());
        std::fflush(stdout);
    }

    void configure_cli_app(CLI::App &app, CliOptions &options, CLI::Option *&resume_flag, CLI::Option *&protect_flag) {
        app.add_option("-k,--api-key", options.api_key, "API key (or configure profiles.<name>.api_key, or set LLM_API_KEY)");
        app.add_option("--model", options.cli_model, "Model to use");
        app.add_option("--agent", options.cli_agent_key, "Configured agent key to use in CLI mode");
        app.add_option("-m,--message", options.message, "Single message mode: send one message, print response, exit");
        app.add_flag("--cli", options.cli_mode, "Start the interactive CLI entry");
        app.add_flag("--web", options.web_mode, "Start the web management UI server");
        app.add_flag("--channel", options.channel_mode, "Start configured channel adapters");
        app.add_flag("--event-stream", options.event_stream, "Emit newline-delimited JSON events (single-message mode only)");
        app.add_flag("--dump-session", options.dump_session, "Emit the resumed session history as NDJSON and exit");
        app.add_option("-r,--resume", options.resume_session, "Resume a saved session (ID, 'latest', or omit to pick)")->expected(0, 1)->default_str("");
        resume_flag = app.get_option("--resume");
        app.add_flag("-v,--verbose", options.verbose, "Enable debug logging");
        app.add_option("--config-password", options.config_password, "Password used to unlock or protect encrypted config secrets");
        app.add_option("--protect-config-secrets", options.protect_config_path,
                       "Rewrite supported plaintext config secrets in place and exit. Optional argument: config path; defaults to ~/.orangutan/config.json")
            ->expected(0, 1)
            ->default_str("");
        protect_flag = app.get_option("--protect-config-secrets");
        app.add_option("--port", options.web_port, "Web server port (default: 18080)");
        app.add_option("--web-host", options.web_host, "Web server bind address (default: 127.0.0.1)");
        app.add_option("--web-dir", options.web_dir, "Path to web frontend static files");
        app.add_option("--permission-mode", options.permission_mode_str, "Permission mode")->envname("ORANGUTAN_PERMISSION_MODE");
        app.add_flag("--dangerously-skip-permissions", options.dangerously_skip_permissions, "Skip all permission checks");
        app.add_option("--allowed-tools", options.allowed_tools_str, "Comma-separated list of allowed tools");
        app.add_option("--disallowed-tools", options.disallowed_tools_str, "Comma-separated list of disallowed tools");
    }

    void configure_logging(bool verbose) {
        auto logger = spdlog::get("orangutan");
        if (logger == nullptr) {
            logger = spdlog::stderr_color_mt("orangutan");
        }
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    }

    std::string format_cli_permission_prompt(const ToolUse &call, const PermissionDecision &decision) {
        std::string prompt = permissions::approval_prompt_message(decision);
        prompt += "\nTool: " + call.name;
        for (const auto &line : permissions::permission_decision_detail_lines(decision)) {
            prompt += "\n" + line;
        }
        return prompt;
    }

    ApprovalCallback make_cli_approval_callback(bool allow_prompting) {
        if (!allow_prompting || isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
            return {};
        }

        return [](const ToolUse &call, const PermissionDecision &decision) {
            const auto prompt_text = format_cli_permission_prompt(call, decision);
            fmt::print("\n{}\nApprove? [y/N]: ", prompt_text);
            std::fflush(stdout);
            std::string answer;
            if (!std::getline(std::cin, answer)) {
                return false;
            }

            auto normalized = utils::ascii_to_lower_copy(answer);
            std::erase_if(normalized, [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
            return normalized == "y" || normalized == "yes";
        };
    }

    bool validate_initial_options(const CliOptions &options) {
        if (!options.cli_mode && !options.web_mode && !options.channel_mode) {
            fmt::println(stderr, "Error: specify at least one entry flag: --cli, --web, or --channel.");
            return false;
        }
        if (options.protect_config_requested && (options.cli_mode || options.web_mode || options.channel_mode || options.resume_requested || !options.message.empty() ||
                                                 options.event_stream || options.dump_session || !options.api_key.empty())) {
            fmt::println(stderr, "Error: --protect-config-secrets cannot be combined with runtime execution flags.");
            return false;
        }
        if (!options.cli_mode && (options.resume_requested || !options.message.empty() || options.event_stream || options.dump_session)) {
            fmt::println(stderr, "Error: --message, --resume, --event-stream, and --dump-session require --cli.");
            return false;
        }
        return true;
    }

    int run_protect_config_mode(const CliOptions &options) {
        const auto path = options.protect_config_path.empty() ? config::default_orangutan_config_path() : std::filesystem::path{options.protect_config_path};
        if (path.empty()) {
            fmt::println(stderr, "Error: could not resolve the default config path.");
            return 1;
        }
        if (!std::filesystem::exists(path)) {
            fmt::println(stderr, "Error: config file not found: {}", path.string());
            return 1;
        }

        try {
            config::ConfigSecretOptions secret_options{
                .password_override = options.config_password,
                .allow_interactive_password = true,
            };
            const auto password = config::resolve_config_secret_password(secret_options);
            const auto result = config::protect_config_file_secrets(path, password);
            if (!result.modified) {
                fmt::println("No eligible plaintext config secrets found in {}.", path.string());
                return 0;
            }

            static_cast<void>(config::Config::load_from(path, config::ConfigSecretOptions{
                                                                  .password_override = password,
                                                              }));

            fmt::println("Protected {} config secret(s) in {}", result.protected_count, path.string());
            fmt::println("Backup written to {}", result.backup_path.string());
            return 0;
        } catch (const config::ConfigSecretProtectionError &e) {
            fmt::println(stderr, "Error: {}", e.what());
            return 1;
        } catch (const std::exception &e) {
            fmt::println(stderr, "Error: {}", e.what());
            return 1;
        }
    }

    CLIPermissionOptions build_cli_permission_options(const CliOptions &options) {
        CLIPermissionOptions cli_perms;
        cli_perms.dangerously_skip_permissions = options.dangerously_skip_permissions;

        if (!options.permission_mode_str.empty()) {
            auto mode_opt = magic_enum::enum_cast<permission_mode>(utils::normalize_enum_token(options.permission_mode_str));
            mode_opt
                .transform([&cli_perms](permission_mode mode) {
                    cli_perms.mode_override = mode;
                    return mode;
                })
                .or_else([&options] {
                    spdlog::warn("unknown --permission-mode '{}', ignoring", options.permission_mode_str);
                    return std::optional<permission_mode>{};
                });
        }

        if (!options.allowed_tools_str.empty()) {
            cli_perms.allowed_tools = utils::split_trimmed(options.allowed_tools_str);
        }
        if (!options.disallowed_tools_str.empty()) {
            cli_perms.disallowed_tools = utils::split_trimmed(options.disallowed_tools_str);
        }

        return cli_perms;
    }

} // namespace orangutan::bootstrap
