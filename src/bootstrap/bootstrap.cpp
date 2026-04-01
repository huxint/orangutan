#include "bootstrap/bootstrap.hpp"

#include "bootstrap/cli-options.hpp"
#include "bootstrap/cli-runtime.hpp"
#include "bootstrap/channel-serve.hpp"
#include "bootstrap/config-builder.hpp"
#include "bootstrap/config-bootstrap.hpp"
#include "bootstrap/app-runtime.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "bootstrap/runtime-control.hpp"
#include "cli/repl.hpp"
#include "cli/session-workflow.hpp"
#include "cli/single-shot.hpp"
#include "tools/registry/tool.hpp"
#include "web/web-server.hpp"
#include "channel/message-queue.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "skills/skill-loader.hpp"
#include "subagent/subagent-manager.hpp"
#include "config/config.hpp"
#include "storage/session-store.hpp"
#include "storage/subagent-run-store.hpp"

#include <CLI/CLI.hpp>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <spdlog/common.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace {

    using orangutan::bootstrap::CliOptions;

    template <typename Store>
    std::unique_ptr<Store> create_store(const char *name) {
        try {
            return std::make_unique<Store>();
        } catch (const std::exception &e) {
            spdlog::fmt_lib::println(stderr, "Error: failed to initialize {}: {}", name, e.what());
            return nullptr;
        }
    }

} // namespace

namespace {

    int run_channel_mode(orangutan::ChannelManager &channel_manager, orangutan::MessageQueue &message_queue,
                         const std::unordered_map<std::string, orangutan::bootstrap::AgentRuntimeConfig> &agent_runtime_configs,
                         const std::unordered_map<std::string, std::string> &qq_bot_agents, orangutan::MemoryStore *memory_store, orangutan::SessionStore &session_store,
                         orangutan::SubagentManager &subagent_manager, const orangutan::Config &cfg, orangutan::HookManager *hook_manager,
                         orangutan::automation::Runtime *automation_runtime) {
        auto &stop_requested = orangutan::bootstrap::signal_stop_requested();
        stop_requested.store(false);
        auto channel_task_runner = std::make_unique<orangutan::JidTaskRunner>(orangutan::bootstrap::default_serve_worker_count());
        spdlog::info("Starting channel executor (configured concurrency hint: {})", channel_task_runner->worker_count());

        if (channel_manager.has_channels()) {
            try {
                channel_manager.connect_all([&message_queue](const orangutan::InboundMessage &message) {
                    message_queue.push(message);
                });
            } catch (const std::exception &e) {
                spdlog::fmt_lib::println(stderr, "Error: failed to start configured channels: {}", e.what());
                channel_manager.disconnect_all();
                return 1;
            }
        } else {
            spdlog::warn("Channel mode started without any configured channels.");
        }

        std::signal(SIGINT, orangutan::bootstrap::handle_signal);
        std::signal(SIGTERM, orangutan::bootstrap::handle_signal);

        orangutan::bootstrap::run_channel_loop(message_queue, channel_manager, stop_requested, *channel_task_runner, agent_runtime_configs, qq_bot_agents, memory_store,
                                               session_store, subagent_manager, cfg, hook_manager, automation_runtime);

        channel_task_runner->shutdown(true);
        channel_manager.disconnect_all();
        return 0;
    }

} // namespace

int orangutan::bootstrap::run(int argc, char **argv) {
    CliOptions options;
    CLI::App app{"orangutan - your local AI assistant"};
    CLI::Option *resume_flag = nullptr;
    CLI::Option *protect_flag = nullptr;
    configure_cli_app(app, options, resume_flag, protect_flag);
    CLI11_PARSE(app, argc, argv);
    options.resume_requested = resume_flag->count() > 0;
    options.protect_config_requested = protect_flag->count() > 0;

    configure_logging(options.verbose);
    if (!validate_initial_options(options)) {
        return 1;
    }
    if (options.protect_config_requested) {
        return run_protect_config_mode(options);
    }

    orangutan::Config cfg;
    try {
        cfg = orangutan::Config::load(orangutan::ConfigSecretOptions{
            .password_override = options.config_password,
            .allow_interactive_password = true,
        });
    } catch (const orangutan::ConfigSecretProtectionError &e) {
        spdlog::fmt_lib::println(stderr, "Error: {}", e.what());
        return 1;
    }
    apply_cli_edit_mode_override(cfg, options.edit_mode);

    std::optional<orangutan::AgentConfig> maybe_selected_agent;
    std::optional<std::string> maybe_workspace;
    std::optional<orangutan::bootstrap::AgentRuntimeConfig> maybe_primary_runtime_cfg;
    std::optional<orangutan::bootstrap::RuntimeIdentity> maybe_primary_identity;
    std::string primary_api_key;

    if (options.cli_mode || options.web_mode) {
        maybe_selected_agent = resolve_selected_agent(cfg, options);
        if (!maybe_selected_agent.has_value()) {
            return 1;
        }

        maybe_workspace = resolve_agent_workspace(*maybe_selected_agent);
        if (!maybe_workspace.has_value()) {
            return 1;
        }

        const auto maybe_endpoint = orangutan::bootstrap::detail::resolve_agent_endpoint(cfg, *maybe_selected_agent, options.cli_agent_key, options.api_key);
        if (!maybe_endpoint.has_value()) {
            return 1;
        }
        primary_api_key = maybe_endpoint->api_key;
        maybe_primary_identity = orangutan::bootstrap::derive_cli_identity(*maybe_workspace, options.cli_agent_key);
        maybe_primary_runtime_cfg = orangutan::bootstrap::AgentRuntimeConfig{
            .agent_key = options.cli_agent_key,
            .provider_name = maybe_endpoint->provider_name,
            .api_key = primary_api_key,
            .model = maybe_selected_agent->model,
            .fallback_models = maybe_selected_agent->fallback_models,
            .base_url = maybe_endpoint->base_url,
            .system_prompt = maybe_selected_agent->system_prompt,
            .workspace_root = *maybe_workspace,
            .edit_mode = maybe_selected_agent->edit_mode,
            .thinking_budget = maybe_selected_agent->thinking_budget,
            .cli_runtime_key = maybe_primary_identity->runtime_key,
            .cli_memory_scope = maybe_primary_identity->memory_scope,
            .memory = cfg.memory,
            .permissions = maybe_selected_agent->permissions,
            .allowed_child_agents = maybe_selected_agent->subagents,
        };
    }

    if (options.cli_mode && primary_api_key.empty()) {
        spdlog::fmt_lib::println(stderr, "Error: missing API key for agent '{}'.", options.cli_agent_key);
        spdlog::fmt_lib::println(stderr, "Set profiles.<name>.api_key, LLM_API_KEY, or use --api-key");
        return 1;
    }

    auto memory_store = create_store<orangutan::MemoryStore>("memory store");
    auto session_store = create_store<orangutan::SessionStore>("session store");
    auto subagent_run_store = create_store<orangutan::SubagentRunStore>("subagent run store");
    if (memory_store == nullptr || session_store == nullptr || subagent_run_store == nullptr) {
        return 1;
    }

    const auto maybe_agent_runtime_configs = detail::build_agent_runtime_configs(cfg, options.api_key);
    if (!maybe_agent_runtime_configs.has_value()) {
        return 1;
    }
    const auto qq_bot_agents = build_qq_bot_agents(cfg);
    const auto subagent_child_runtime_configs = detail::build_subagent_child_runtime_configs(*maybe_agent_runtime_configs);
    orangutan::SubagentManager subagent_manager(*subagent_run_store, orangutan::SubagentExecutionEnvironment{
                                                                         .agent_configs = &subagent_child_runtime_configs,
                                                                         .session_store = session_store.get(),
                                                                         .memory_store = memory_store.get(),
                                                                     });
    orangutan::bootstrap::AppRuntime app_runtime;

    app_runtime.automation_runtime().set_executor(
        [&cfg, &subagent_manager, &app_runtime, &maybe_agent_runtime_configs, memory_store = memory_store.get()](const orangutan::automation::Trigger &trigger) {
            orangutan::automation::ExecutionResult result;
            auto config_it = maybe_agent_runtime_configs->find(trigger.agent_key);
            if (config_it == maybe_agent_runtime_configs->end()) {
                result.summary = "No runtime configuration for agent '" + trigger.agent_key + "'.";
                return result;
            }

            const auto &runtime_cfg = config_it->second;
            std::string current_session_id;
            auto completion_resume_state = std::make_shared<RuntimeCompletionResumeState>();
            completion_resume_state->agent_key = runtime_cfg.agent_key;
            completion_resume_state->configured_model = runtime_cfg.model;
            completion_resume_state->scope_key = "agent:" + runtime_cfg.agent_key + "|automation";
            completion_resume_state->automation_runtime = &app_runtime.automation_runtime();
            completion_resume_state->suppress_human_output = true;
            orangutan::bootstrap::RuntimeIdentity identity{
                .workspace = runtime_cfg.workspace_root,
                .runtime_key = "agent:" + runtime_cfg.agent_key + "|automation:" + trigger.automation_id,
                .memory_scope = "agent:" + runtime_cfg.agent_key + "|automation",
            };

            try {
                auto runtime = orangutan::bootstrap::build_agent_runtime(orangutan::bootstrap::AgentRuntimeBuildInput{
                    .provider_name = runtime_cfg.provider_name,
                    .api_key = runtime_cfg.api_key,
                    .model = runtime_cfg.model,
                    .fallback_models = runtime_cfg.fallback_models,
                    .base_url = runtime_cfg.base_url,
                    .agent_key = runtime_cfg.agent_key,
                    .system_prompt = runtime_cfg.system_prompt,
                    .workspace_root = runtime_cfg.workspace_root,
                    .edit_mode = runtime_cfg.edit_mode,
                    .thinking_budget = runtime_cfg.thinking_budget,
                    .memory = runtime_cfg.memory,
                    .permissions = runtime_cfg.permissions,
                    .allowed_child_agents = runtime_cfg.allowed_child_agents,
                    .identity = identity,
                    .memory_store = memory_store,
                    .current_session_id = &current_session_id,
                    .subagent_manager = &subagent_manager,
                    .runtime_origin = base::origin::cli,
                    .raw_caller_id = identity.runtime_key,
                    .automation_runtime = &app_runtime.automation_runtime(),
                    .custom_tools = cfg.custom_tools,
                    .mcp_servers = cfg.mcp_servers,
                    .skill_paths = cfg.skill_paths,
                    .hook_paths = cfg.hook_paths,
                    .background_completion_runtime =
                        make_runtime_background_completion_bindings(&app_runtime.automation_runtime(), make_runtime_completion_resume_callback(completion_resume_state)),
                });
                RuntimeCompletionResumeStateGuard completion_resume_guard{completion_resume_state};
                completion_resume_state->agent = runtime.agent.get();
                completion_resume_state->provider = runtime.provider.get();
                result.reply = runtime.agent->run(trigger.prompt);
                result.summary = result.reply;
                result.workspace_root = runtime_cfg.workspace_root;
                result.success = true;
                return result;
            } catch (const std::exception &e) {
                result.summary = e.what();
                result.workspace_root = runtime_cfg.workspace_root;
                return result;
            }
        });

    orangutan::ChannelManager channel_manager(orangutan::Allowlist(cfg.allow, cfg.deny));
    orangutan::bootstrap::add_configured_channels(channel_manager, cfg);
    app_runtime.automation_runtime().set_notifier([&channel_manager](std::string_view target, std::string_view message) -> std::optional<std::string> {
        try {
            channel_manager.send(std::string(target), std::string(message));
            return std::nullopt;
        } catch (const std::exception &e) {
            return e.what();
        }
    });
    app_runtime.automation_runtime().start();

    std::unique_ptr<orangutan::bootstrap::AgentRuntimeBundle> primary_runtime;
    orangutan::SkillLoader skill_loader;
    std::string current_session_id;
    std::string web_runtime_build_error;
    std::shared_ptr<RuntimeCompletionResumeState> primary_completion_resume_state;
    if (maybe_primary_runtime_cfg.has_value()) {
        load_display_skills(skill_loader, cfg, maybe_primary_runtime_cfg->workspace_root);
    }
    if (maybe_primary_runtime_cfg.has_value() && !primary_api_key.empty()) {
        try {
            primary_completion_resume_state = std::make_shared<RuntimeCompletionResumeState>();
            primary_completion_resume_state->agent_key = maybe_primary_runtime_cfg->agent_key;
            primary_completion_resume_state->configured_model = maybe_primary_runtime_cfg->model;
            primary_completion_resume_state->scope_key = maybe_primary_runtime_cfg->cli_memory_scope;
            primary_completion_resume_state->session_store = session_store.get();
            primary_completion_resume_state->current_session_id = &current_session_id;
            primary_completion_resume_state->automation_runtime = &app_runtime.automation_runtime();
            primary_completion_resume_state->persist_session = cfg.auto_save;
            orangutan::bootstrap::detail::maybe_inject_web_runtime_build_failure_for_tests();
            const auto approval_callback = make_cli_approval_callback(!options.event_stream);
            primary_runtime = std::make_unique<orangutan::bootstrap::AgentRuntimeBundle>(orangutan::bootstrap::build_agent_runtime(orangutan::bootstrap::AgentRuntimeBuildInput{
                .provider_name = maybe_primary_runtime_cfg->provider_name,
                .api_key = maybe_primary_runtime_cfg->api_key,
                .model = maybe_primary_runtime_cfg->model,
                .fallback_models = maybe_primary_runtime_cfg->fallback_models,
                .base_url = maybe_primary_runtime_cfg->base_url,
                .agent_key = maybe_primary_runtime_cfg->agent_key,
                .system_prompt = maybe_primary_runtime_cfg->system_prompt,
                .workspace_root = maybe_primary_runtime_cfg->workspace_root,
                .edit_mode = maybe_primary_runtime_cfg->edit_mode,
                .memory = maybe_primary_runtime_cfg->memory,
                .permissions = maybe_primary_runtime_cfg->permissions,
                .allowed_child_agents = maybe_primary_runtime_cfg->allowed_child_agents,
                .identity = *maybe_primary_identity,
                .memory_store = memory_store.get(),
                .current_session_id = &current_session_id,
                .subagent_manager = &subagent_manager,
                .runtime_origin = base::origin::cli,
                .raw_caller_id = "cli:local",
                .automation_runtime = &app_runtime.automation_runtime(),
                .approval_callback = approval_callback,
                .custom_tools = cfg.custom_tools,
                .mcp_servers = cfg.mcp_servers,
                .skill_paths = cfg.skill_paths,
                .hook_paths = cfg.hook_paths,
                .background_completion_runtime =
                    make_runtime_background_completion_bindings(&app_runtime.automation_runtime(), make_runtime_completion_resume_callback(primary_completion_resume_state)),
            }));
            primary_completion_resume_state->agent = primary_runtime->agent.get();
            primary_completion_resume_state->provider = primary_runtime->provider.get();
            primary_completion_resume_state->hook_manager = primary_runtime->hook_manager.get();
            log_loaded_hooks(resolve_runtime_hook_dirs(cfg, maybe_primary_runtime_cfg->workspace_root), *primary_runtime->hook_manager);
        } catch (const std::exception &e) {
            web_runtime_build_error = e.what();
            if (options.web_mode && !options.cli_mode) {
                spdlog::warn("Web runtime assembly failed; continuing with admin-only web surface: {}", e.what());
            } else {
                spdlog::fmt_lib::println(stderr, "Error: failed to initialize primary runtime: {}", e.what());
                app_runtime.automation_runtime().stop();
                return 1;
            }
        }
    }
    RuntimeCompletionResumeStateGuard primary_completion_resume_guard{primary_completion_resume_state};

    std::unique_ptr<orangutan::WebServer> web_server;
    if (options.web_mode) {
        web_server = std::make_unique<orangutan::WebServer>();
        const auto attachments = configure_web_server_runtime(*web_server, options, cfg, session_store.get(), memory_store.get(), &subagent_manager,
                                                              &app_runtime.automation_runtime(), primary_runtime != nullptr ? &primary_runtime->tools : nullptr, &skill_loader);
        if (maybe_skip_web_server_start_for_tests(session_store.get(), memory_store.get(), subagent_run_store.get(), &subagent_manager, primary_runtime.get(), &skill_loader,
                                                  attachments, web_runtime_build_error)) {
            app_runtime.automation_runtime().stop();
            return 0;
        }
        warn_if_nonlocal_web_host(options.web_host);
        web_server->start(options.web_host, options.web_port);
        spdlog::fmt_lib::println("Web UI available at http://{}:{}", options.web_host, web_server->port());
    }

    orangutan::MessageQueue message_queue;
    std::thread channel_thread;
    auto &stop_requested = signal_stop_requested();
    stop_requested.store(false);
    std::atomic<int> channel_exit_code{0};
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (options.channel_mode) {
        channel_thread = std::thread([&] {
            int status = 0;
            if (auto &callback = detail::channel_mode_callback(); callback) {
                status = callback();
            } else {
                status = run_channel_mode(channel_manager, message_queue, *maybe_agent_runtime_configs, qq_bot_agents, memory_store.get(), *session_store, subagent_manager, cfg,
                                          nullptr, &app_runtime.automation_runtime());
            }
            channel_exit_code.store(status);
            if (status != 0) {
                stop_requested.store(true);
            }
        });
    }

    int exit_code = 0;
    if (options.cli_mode) {
        if (primary_runtime == nullptr || !maybe_primary_runtime_cfg.has_value()) {
            spdlog::fmt_lib::println(stderr, "Error: failed to initialize CLI runtime.");
            exit_code = 1;
        } else {
            auto resume_session = options.resume_session;
            if (!restore_requested_session(options, *session_store, *maybe_primary_runtime_cfg, *primary_runtime->agent, resume_session, current_session_id)) {
                exit_code = 1;
            } else {
                options.message = merge_stdin_message(options.message);
                if (!validate_runtime_mode_options(options, !current_session_id.empty())) {
                    exit_code = 1;
                } else if (options.dump_session) {
                    orangutan::cli::emit_session_history_dump(primary_runtime->agent->history(), current_session_id, emit_json_event);
                } else if (!options.message.empty()) {
                    exit_code = orangutan::cli::run_single_message(*primary_runtime->agent, *primary_runtime->provider, *session_store, cfg, options.message, options.event_stream,
                                                                   current_session_id, maybe_primary_runtime_cfg->model, maybe_primary_runtime_cfg->cli_memory_scope,
                                                                   maybe_primary_runtime_cfg->agent_key, emit_json_event, std::cerr, &app_runtime.automation_runtime());
                } else {
                    orangutan::cli::run_repl(*primary_runtime->agent, *primary_runtime->provider, *session_store, maybe_primary_runtime_cfg->model,
                                             maybe_primary_runtime_cfg->fallback_models, cfg, current_session_id, maybe_primary_runtime_cfg->agent_key,
                                             maybe_primary_runtime_cfg->cli_memory_scope, maybe_primary_runtime_cfg->workspace_root, &skill_loader, &primary_runtime->tools,
                                             primary_runtime->hook_manager.get(), &app_runtime.automation_runtime());
                }
            }
        }
    } else {
        while (!stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    stop_requested.store(true);
    message_queue.shutdown();
    if (channel_thread.joinable()) {
        channel_thread.join();
    }
    if (exit_code == 0 && channel_exit_code.load() != 0) {
        exit_code = channel_exit_code.load();
    }
    if (web_server != nullptr) {
        web_server->stop();
    }
    app_runtime.automation_runtime().stop();
    return exit_code;
}
