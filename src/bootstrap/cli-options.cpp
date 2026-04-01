#include "bootstrap/cli-options.hpp"

#include "config/config.hpp"
#include "config/secret-protection.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>

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
        spdlog::fmt_lib::println("{}", event.dump());
        std::fflush(stdout);
    }

    void configure_cli_app(CLI::App &app, CliOptions &options, CLI::Option *&resume_flag, CLI::Option *&protect_flag) {
        app.add_option("-k,--api-key", options.api_key, "API key (or configure agent.api_key, or set ANTHROPIC_API_KEY / LLM_API_KEY env)");
        app.add_option("--model", options.cli_model, "Model to use");
        app.add_option("-b,--base-url", options.cli_base_url, "API base URL");
        app.add_option("-p,--provider", options.cli_provider, "LLM provider (anthropic, openai)");
        app.add_option("--agent", options.cli_agent_key, "Configured agent key to use in CLI mode");
        app.add_option("-m,--message", options.message, "Single message mode: send one message, print response, exit");
        app.add_option("-s,--system-prompt", options.cli_system_prompt, "Custom system prompt");
        app.add_flag("--cli", options.cli_mode, "Start the interactive CLI entry");
        app.add_flag("--web", options.web_mode, "Start the web management UI server");
        app.add_flag("--channel", options.channel_mode, "Start configured channel adapters");
        app.add_flag("--event-stream", options.event_stream, "Emit newline-delimited JSON events (single-message mode only)");
        app.add_flag("--dump-session", options.dump_session, "Emit the resumed session history as NDJSON and exit");
        app.add_option("-r,--resume", options.resume_session, "Resume a saved session (ID, 'latest', or omit to pick)")->expected(0, 1)->default_str("");
        resume_flag = app.get_option("--resume");
        app.add_flag("-v,--verbose", options.verbose, "Enable debug logging");
        app.add_option("--edit-mode", options.edit_mode, "Edit tool mode: hashline or search_replace");
        app.add_option("--config-password", options.config_password, "Password used to unlock or protect encrypted config secrets");
        app.add_option("--protect-config-secrets", options.protect_config_path,
                       "Rewrite supported plaintext config secrets in place and exit. Optional argument: config path; defaults to ~/.orangutan/config.json")
            ->expected(0, 1)
            ->default_str("");
        protect_flag = app.get_option("--protect-config-secrets");
        app.add_option("--port", options.web_port, "Web server port (default: 18080)");
        app.add_option("--web-host", options.web_host, "Web server bind address (default: 127.0.0.1)");
        app.add_option("--web-dir", options.web_dir, "Path to web frontend static files");
    }

    void configure_logging(bool verbose) {
        auto logger = spdlog::get("orangutan");
        if (logger == nullptr) {
            logger = spdlog::stderr_color_mt("orangutan");
        }
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    }

    ToolApprovalCallback make_cli_approval_callback(bool allow_prompting) {
        if (!allow_prompting || isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
            return {};
        }

        return [](const ToolUse & /*call*/, const std::string &prompt_text) {
            spdlog::fmt_lib::print("\n{}\nApprove? [y/N]: ", prompt_text);
            std::fflush(stdout);
            std::string answer;
            if (!std::getline(std::cin, answer)) {
                return false;
            }

            std::string normalized;
            normalized.reserve(answer.size());
            for (const auto ch : answer) {
                if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
                    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                }
            }
            return normalized == "y" || normalized == "yes";
        };
    }

    bool validate_initial_options(const CliOptions &options) {
        if (!options.cli_mode && !options.web_mode && !options.channel_mode) {
            spdlog::fmt_lib::println(stderr, "Error: specify at least one entry flag: --cli, --web, or --channel.");
            return false;
        }
        if (options.protect_config_requested && (options.cli_mode || options.web_mode || options.channel_mode || options.resume_requested || !options.message.empty() ||
                                                 options.event_stream || options.dump_session || !options.api_key.empty())) {
            spdlog::fmt_lib::println(stderr, "Error: --protect-config-secrets cannot be combined with runtime execution flags.");
            return false;
        }
        if (!options.cli_mode && (options.resume_requested || !options.message.empty() || options.event_stream || options.dump_session)) {
            spdlog::fmt_lib::println(stderr, "Error: --message, --resume, --event-stream, and --dump-session require --cli.");
            return false;
        }
        return true;
    }

    int run_protect_config_mode(const CliOptions &options) {
        const auto path = options.protect_config_path.empty() ? config::default_orangutan_config_path() : std::filesystem::path{options.protect_config_path};
        if (path.empty()) {
            spdlog::fmt_lib::println(stderr, "Error: could not resolve the default config path.");
            return 1;
        }
        if (!std::filesystem::exists(path)) {
            spdlog::fmt_lib::println(stderr, "Error: config file not found: {}", path.string());
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
                spdlog::fmt_lib::println("No eligible plaintext config secrets found in {}.", path.string());
                return 0;
            }

            static_cast<void>(config::Config::load_from(path, config::ConfigSecretOptions{
                                                                  .password_override = password,
                                                              }));

            spdlog::fmt_lib::println("Protected {} config secret(s) in {}", result.protected_count, path.string());
            spdlog::fmt_lib::println("Backup written to {}", result.backup_path.string());
            return 0;
        } catch (const config::ConfigSecretProtectionError &e) {
            spdlog::fmt_lib::println(stderr, "Error: {}", e.what());
            return 1;
        }
    }

} // namespace orangutan::bootstrap
