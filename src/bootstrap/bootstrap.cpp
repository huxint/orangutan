#include "bootstrap/bootstrap.hpp"

#include "bootstrap/cli-options.hpp"
#include "bootstrap/cli-runtime.hpp"
#include "bootstrap/channel-serve.hpp"
#include "bootstrap/config-builder.hpp"
#include "bootstrap/config-bootstrap.hpp"
#include "bootstrap/app-runtime.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "bootstrap/runtime-assembler.hpp"
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
#include "config/config.hpp"
#include "storage/session-store.hpp"
#include "coordinator/coordinator-manager.hpp"
#include "coordinator/agent-definition-registry.hpp"
#include "swarm/mailbox.hpp"
#include "swarm/team-manager.hpp"
#include "utils/escape.hpp"
#include "utils/scope-exit.hpp"

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

    std::vector<std::string> fallback_labels(const std::vector<orangutan::config::FallbackModelRef> &fallback_models) {
        std::vector<std::string> labels;
        labels.reserve(fallback_models.size());
        for (const auto &fallback : fallback_models) {
            if (fallback.profile.empty()) {
                labels.push_back(fallback.model);
            } else {
                labels.push_back(fallback.profile + ":" + fallback.model);
            }
        }
        return labels;
    }

    template <typename Store>
    std::unique_ptr<Store> create_store(const char *name) {
        try {
            return std::make_unique<Store>();
        } catch (const std::exception &e) {
            spdlog::fmt_lib::println(stderr, "Error: failed to initialize {}: {}", name, e.what());
            return nullptr;
        }
    }

    std::optional<std::string> resolve_app_workspace_root(const std::optional<orangutan::bootstrap::AgentRuntimeConfig> &maybe_primary_runtime_cfg,
                                                          const std::unordered_map<std::string, orangutan::bootstrap::AgentRuntimeConfig> &agent_runtime_configs) {
        if (maybe_primary_runtime_cfg.has_value()) {
            return maybe_primary_runtime_cfg->workspace_root;
        }
        if (agent_runtime_configs.empty()) {
            return std::nullopt;
        }

        const auto &workspace_root = agent_runtime_configs.begin()->second.workspace_root;
        for (const auto &[agent_key, runtime_cfg] : agent_runtime_configs) {
            if (runtime_cfg.workspace_root != workspace_root) {
                spdlog::fmt_lib::println(stderr, "Error: agent '{}' uses workspace '{}', which does not match the shared workspace '{}'.", agent_key, runtime_cfg.workspace_root,
                                         workspace_root);
                return std::nullopt;
            }
        }
        return workspace_root;
    }

    std::string format_teammate_message_xml(const orangutan::MailboxMessage &message) {
        return "<teammate-message from=\"" + orangutan::utils::escape_xml(message.from) + "\">" + orangutan::utils::escape_xml(message.text) + "</teammate-message>";
    }

} // namespace

namespace {

    int run_channel_mode(orangutan::ChannelManager &channel_manager, orangutan::MessageQueue &message_queue,
                         const std::unordered_map<std::string, orangutan::bootstrap::AgentRuntimeConfig> &agent_runtime_configs,
                         const orangutan::utils::transparent_string_unordered_map<std::string> &qq_bot_agents, orangutan::MemoryStore *memory_store,
                         orangutan::SessionStore &session_store, orangutan::coordinator::CoordinatorManager *coordinator_manager, const orangutan::Config &cfg,
                         orangutan::HookManager *hook_manager, orangutan::automation::Runtime *automation_runtime, orangutan::swarm::TeamManager *team_manager,
                         orangutan::swarm::AgentMailbox *mailbox) {
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
                                               session_store, coordinator_manager, cfg, hook_manager, automation_runtime, team_manager, mailbox);

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
    const auto cli_permission_options = build_cli_permission_options(options);

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

        const auto maybe_route = orangutan::bootstrap::detail::resolve_agent_route(cfg, *maybe_selected_agent, options.cli_agent_key, options.api_key);
        if (!maybe_route.has_value()) {
            return 1;
        }
        primary_api_key = maybe_route->route.primary.api_key;
        maybe_primary_identity = orangutan::bootstrap::derive_cli_identity(*maybe_workspace, options.cli_agent_key);
        maybe_primary_runtime_cfg = orangutan::bootstrap::AgentRuntimeConfig{
            .agent_key = options.cli_agent_key,
            .model = maybe_selected_agent->model,
            .fallback_models = fallback_labels(maybe_selected_agent->fallback_models),
            .provider_route = maybe_route->route,
            .workspace_root = *maybe_workspace,
            .edit_mode = maybe_selected_agent->edit_mode,
            .thinking_budget = maybe_selected_agent->thinking_budget,
            .cli_runtime_key = maybe_primary_identity->runtime_key,
            .cli_memory_scope = maybe_primary_identity->memory_scope,
            .memory = cfg.memory,
            .permission_context = initialize_permission_context(maybe_selected_agent->permissions_config, cli_permission_options, *maybe_workspace),
            .team_agents = maybe_selected_agent->team_agents,
        };
    }

    if (options.cli_mode && primary_api_key.empty()) {
        spdlog::fmt_lib::println(stderr, "Error: missing API key for agent '{}'.", options.cli_agent_key);
        spdlog::fmt_lib::println(stderr, "Set profiles.<name>.api_key, LLM_API_KEY, or use --api-key");
        return 1;
    }

    const auto maybe_agent_runtime_configs = detail::build_agent_runtime_configs(cfg, options.api_key, cli_permission_options);
    if (!maybe_agent_runtime_configs.has_value()) {
        return 1;
    }
    const auto maybe_app_workspace_root = resolve_app_workspace_root(maybe_primary_runtime_cfg, *maybe_agent_runtime_configs);
    if (!maybe_app_workspace_root.has_value()) {
        spdlog::fmt_lib::println(stderr, "Error: unable to resolve a shared workspace root for runtime state.");
        return 1;
    }

    std::unique_ptr<orangutan::MemoryStore> memory_store;
    std::unique_ptr<orangutan::SessionStore> session_store;
    try {
        memory_store = std::make_unique<orangutan::MemoryStore>(orangutan::bootstrap::workspace_memory_store_path(*maybe_app_workspace_root));
        session_store = std::make_unique<orangutan::SessionStore>(orangutan::bootstrap::workspace_session_store_path(*maybe_app_workspace_root));
    } catch (const std::exception &e) {
        spdlog::fmt_lib::println(stderr, "Error: failed to initialize runtime stores: {}", e.what());
        return 1;
    }

    const auto qq_bot_agents = build_qq_bot_agents(cfg);

    // Create coordinator components
    auto coordinator_state_root = orangutan::bootstrap::workspace_state_root(*maybe_app_workspace_root);
    auto agent_definition_registry = std::make_unique<orangutan::coordinator::AgentDefinitionRegistry>();
    agent_definition_registry->load_builtin_definitions();
    if (maybe_primary_runtime_cfg.has_value()) {
        agent_definition_registry->load_from_directory(std::filesystem::path{maybe_primary_runtime_cfg->workspace_root} / ".orangutan" / "agents");
    }

    std::unique_ptr<orangutan::swarm::AgentMailbox> agent_mailbox;
    std::unique_ptr<orangutan::swarm::TeamManager> team_manager;
    try {
        agent_mailbox = std::make_unique<orangutan::swarm::AgentMailbox>((coordinator_state_root / "mailbox.db").string());
        team_manager = std::make_unique<orangutan::swarm::TeamManager>((coordinator_state_root / "teams.db").string());
    } catch (const std::exception &e) {
        spdlog::warn("Failed to initialize coordinator stores: {}", e.what());
    }

    int coordinator_max_concurrent = 4;
    if (maybe_selected_agent.has_value()) {
        coordinator_max_concurrent = maybe_selected_agent->max_concurrent_agents;
    }
    auto coordinator_manager = std::make_unique<orangutan::coordinator::CoordinatorManager>(coordinator_max_concurrent);
    coordinator_manager->set_environment(orangutan::coordinator::AgentExecutionEnvironment{
        .definition_registry = agent_definition_registry.get(),
        .session_store = session_store.get(),
        .memory_store = memory_store.get(),
        .mailbox = agent_mailbox.get(),
        .team_manager = team_manager.get(),
    });

    coordinator_manager->set_worker_runtime_factory([&cfg, &maybe_agent_runtime_configs, memory_store = memory_store.get(), &coordinator_manager](
                                                        const orangutan::coordinator::AgentSpawnRequest &request) -> std::unique_ptr<orangutan::coordinator::WorkerRuntime> {
        auto config_it = maybe_agent_runtime_configs->find(request.agent_key);
        if (config_it == maybe_agent_runtime_configs->end()) {
            throw std::runtime_error("No runtime configuration for agent '" + request.agent_key + "'.");
        }

        const auto &runtime_cfg = config_it->second;

        struct RuntimeWorker : orangutan::coordinator::WorkerRuntime {
            orangutan::bootstrap::AgentRuntimeBundle bundle;
            std::string session_id;
            std::string team_id;
            std::string agent_name;

            explicit RuntimeWorker(orangutan::bootstrap::AgentRuntimeBundle b)
            : bundle(std::move(b)) {}

            std::string run(const std::string &prompt, std::stop_token stop_token) override {
                bundle.agent->set_stop_requested_callback([stop_token]() {
                    return stop_token.stop_requested();
                });

                if (bundle.tool_context().mailbox != nullptr && !team_id.empty() && !agent_name.empty()) {
                    auto *mailbox = bundle.tool_context().mailbox;
                    const auto team = team_id;
                    const auto name = agent_name;
                    bundle.agent->set_incoming_message_fetcher([mailbox, team, name]() {
                        auto messages = mailbox->poll(team, name);
                        if (messages.empty()) {
                            return std::vector<std::string>{};
                        }

                        std::vector<std::string> injected;
                        std::vector<std::string> ids;
                        injected.reserve(messages.size());
                        ids.reserve(messages.size());
                        for (const auto &message : messages) {
                            injected.push_back(format_teammate_message_xml(message));
                            ids.push_back(message.id);
                        }
                        mailbox->mark_read(ids);
                        return injected;
                    });
                }

                return bundle.agent->run(prompt);
            }
        };

        orangutan::bootstrap::RuntimeIdentity identity{
            .workspace = runtime_cfg.workspace_root,
            .runtime_key = "agent:" + runtime_cfg.agent_key + "|worker:" + request.agent_name,
            .memory_scope = "agent:" + runtime_cfg.agent_key + "|worker",
        };

        auto bundle = orangutan::bootstrap::build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
            .runtime_config = &runtime_cfg,
            .identity = &identity,
            .app_config = &cfg,
            .memory_store = memory_store,
            .agent_name = request.agent_name,
            .team_agents = std::vector<std::string>{},
            .team_id = request.team_id,
            .coordinator_manager = coordinator_manager.get(),
            .is_child_run = true,
            .delegated_task_prompt = request.task_prompt,
        }));

        auto worker = std::make_unique<RuntimeWorker>(std::move(bundle));
        worker->team_id = request.team_id;
        worker->agent_name = request.agent_name;
        return worker;
    });

    orangutan::bootstrap::AppRuntime app_runtime(orangutan::bootstrap::workspace_automation_store_path(*maybe_app_workspace_root));

    app_runtime.automation_runtime().set_executor([&cfg, &app_runtime, &maybe_agent_runtime_configs, memory_store = memory_store.get(), &coordinator_manager, &team_manager,
                                                   &agent_mailbox](const orangutan::automation::Trigger &trigger) {
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
            auto runtime = orangutan::bootstrap::build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
                .runtime_config = &runtime_cfg,
                .identity = &identity,
                .app_config = &cfg,
                .memory_store = memory_store,
                .current_session_id = &current_session_id,
                .coordinator_manager = coordinator_manager.get(),
                .team_manager = team_manager.get(),
                .mailbox = agent_mailbox.get(),
                .runtime_origin = base::origin::cli,
                .raw_caller_id = identity.runtime_key,
                .automation_runtime = &app_runtime.automation_runtime(),
                .background_completion_runtime =
                    make_runtime_background_completion_bindings(&app_runtime.automation_runtime(), make_runtime_completion_resume_callback(completion_resume_state)),
            }));
            const auto completion_resume_guard = orangutan::utils::scope_exit([completion_resume_state] {
                deactivate_runtime_completion_resume_state(completion_resume_state);
            });
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
            primary_runtime = std::make_unique<orangutan::bootstrap::AgentRuntimeBundle>(orangutan::bootstrap::build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
                .runtime_config = &*maybe_primary_runtime_cfg,
                .identity = &*maybe_primary_identity,
                .app_config = &cfg,
                .memory_store = memory_store.get(),
                .current_session_id = &current_session_id,
                .coordinator_manager = coordinator_manager.get(),
                .team_manager = team_manager.get(),
                .mailbox = agent_mailbox.get(),
                .runtime_origin = base::origin::cli,
                .raw_caller_id = "cli:local",
                .automation_runtime = &app_runtime.automation_runtime(),
                .approval_callback = approval_callback,
                .background_completion_runtime =
                    make_runtime_background_completion_bindings(&app_runtime.automation_runtime(), make_runtime_completion_resume_callback(primary_completion_resume_state)),
            })));
            primary_completion_resume_state->agent = primary_runtime->agent.get();
            primary_completion_resume_state->provider = primary_runtime->provider.get();
            primary_completion_resume_state->hook_manager = primary_runtime->hook_manager.get();
            coordinator_manager->register_runtime_notification_handler(maybe_primary_identity->runtime_key,
                                                                       make_runtime_completion_resume_callback(primary_completion_resume_state));
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
    const auto primary_completion_resume_guard = orangutan::utils::scope_exit([primary_completion_resume_state] {
        deactivate_runtime_completion_resume_state(primary_completion_resume_state);
    });

    std::unique_ptr<orangutan::WebServer> web_server;
    if (options.web_mode) {
        web_server = std::make_unique<orangutan::WebServer>();
        const auto attachments = configure_web_server_runtime(*web_server, options, cfg, session_store.get(), memory_store.get(), &app_runtime.automation_runtime(),
                                                              primary_runtime != nullptr ? &primary_runtime->tools() : nullptr, &skill_loader);
        if (maybe_skip_web_server_start_for_tests(session_store.get(), memory_store.get(), primary_runtime.get(), &skill_loader, attachments, web_runtime_build_error)) {
            app_runtime.automation_runtime().stop();
            return 0;
        }
        warn_if_nonlocal_web_host(options.web_host);
        web_server->start(options.web_host, options.web_port);
        spdlog::fmt_lib::println("Web UI available at http://{}:{}", options.web_host, web_server->port());
    }

    orangutan::MessageQueue message_queue;
    std::jthread channel_thread;
    auto &stop_requested = signal_stop_requested();
    stop_requested.store(false);
    std::atomic<int> channel_exit_code{0};
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (options.channel_mode) {
        channel_thread = std::jthread([&](const std::stop_token &token) {
            int status = 0;
            if (auto &callback = detail::channel_mode_callback(); callback) {
                status = callback();
            } else {
                status = run_channel_mode(channel_manager, message_queue, *maybe_agent_runtime_configs, qq_bot_agents, memory_store.get(), *session_store,
                                          coordinator_manager.get(), cfg, nullptr, &app_runtime.automation_runtime(), team_manager.get(), agent_mailbox.get());
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
                                             maybe_primary_runtime_cfg->cli_memory_scope, maybe_primary_runtime_cfg->workspace_root, &skill_loader, &primary_runtime->tools(),
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
    coordinator_manager->shutdown();
    app_runtime.automation_runtime().stop();
    return exit_code;
}
