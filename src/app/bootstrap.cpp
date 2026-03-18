#include "app/bootstrap.hpp"

#include "app/channel-serve.hpp"
#include "app/repl.hpp"
#include "app/single-shot.hpp"
#include "app/runtime/identity.hpp"
#include "app/runtime/memory-context.hpp"
#include "core/providers/provider.hpp"
#include "core/tools/tool.hpp"
#include "features/agent/agent-loop.hpp"
#include "features/channel/core/message-queue.hpp"
#include "features/cron/parser.hpp"
#include "features/cron/store.hpp"
#include "features/heartbeat/scheduler.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "features/tools/runtime/runtime-loader.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace {

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

std::string resolve_api_key(const std::string &cli_key, const orangutan::Config &cfg) {
    if (!cli_key.empty()) {
        return cli_key;
    }
    const char *env_key = std::getenv("ANTHROPIC_API_KEY");
    if (env_key == nullptr) {
        env_key = std::getenv("LLM_API_KEY");
    }
    if (env_key != nullptr) {
        return env_key;
    }
    if (!cfg.api_key.empty()) {
        return cfg.api_key;
    }
    return {};
}

void emit_json_event(const orangutan::json &event) {
    std::cout << event.dump() << '\n' << std::flush;
}

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
    bool serve_mode = false;
    std::string edit_mode;
    bool verbose = false;
    bool resume_requested = false;
};

std::atomic<bool> &signal_stop_requested() {
    static std::atomic<bool> stop_requested{false};
    return stop_requested;
}

void handle_signal(int /*signum*/) {
    signal_stop_requested().store(true);
}

void configure_cli_app(CLI::App &app, CliOptions &options, CLI::Option *&resume_flag) {
    app.add_option("-k,--api-key", options.api_key, "API key (or set ANTHROPIC_API_KEY / LLM_API_KEY env)");
    app.add_option("--model", options.cli_model, "Model to use");
    app.add_option("-b,--base-url", options.cli_base_url, "API base URL");
    app.add_option("-p,--provider", options.cli_provider, "LLM provider (anthropic, openai)");
    app.add_option("--agent", options.cli_agent_key, "Configured agent key to use in CLI mode");
    app.add_option("-m,--message", options.message, "Single message mode: send one message, print response, exit");
    app.add_option("-s,--system-prompt", options.cli_system_prompt, "Custom system prompt");
    app.add_flag("--serve", options.serve_mode, "Run configured channels without starting the interactive REPL");
    app.add_flag("--event-stream", options.event_stream, "Emit newline-delimited JSON events (single-message mode only)");
    app.add_flag("--dump-session", options.dump_session, "Emit the resumed session history as NDJSON and exit");
    app.add_option("-r,--resume", options.resume_session, "Resume a saved session (ID, 'latest', or omit to pick)")->expected(0, 1)->default_str("");
    resume_flag = app.get_option("--resume");
    app.add_flag("-v,--verbose", options.verbose, "Enable debug logging");
    app.add_option("--edit-mode", options.edit_mode, "Edit tool mode: hashline or search_replace");
}

void configure_logging(bool verbose) {
    spdlog::set_default_logger(spdlog::stderr_color_mt("orangutan"));
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
}

orangutan::ToolApprovalCallback make_cli_approval_callback(bool allow_prompting) {
    if (!allow_prompting || isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
        return {};
    }

    return [](const orangutan::ToolUseBlock &, const std::string &prompt_text) {
        std::cout << "\n" << prompt_text << "\nApprove? [y/N]: " << std::flush;
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
    if (options.serve_mode && (!options.message.empty() || options.event_stream || options.dump_session)) {
        std::cerr << "Error: --serve cannot be combined with single-message or event-stream flags.\n";
        return false;
    }
    return true;
}

std::optional<orangutan::AgentConfig> resolve_selected_agent(const orangutan::Config &cfg, const CliOptions &options) {
    const auto maybe_selected_agent = cfg.find_agent(options.cli_agent_key);
    if (!maybe_selected_agent.has_value()) {
        std::cerr << "Error: unknown agent: " << options.cli_agent_key << '\n';
        return std::nullopt;
    }

    auto selected_agent = *maybe_selected_agent;
    if (!options.cli_provider.empty()) {
        selected_agent.provider = options.cli_provider;
    }
    if (!options.cli_model.empty()) {
        selected_agent.model = options.cli_model;
    }
    if (!options.cli_base_url.empty()) {
        selected_agent.base_url = options.cli_base_url;
    }
    if (!options.cli_system_prompt.empty()) {
        selected_agent.system_prompt = options.cli_system_prompt;
    }
    return selected_agent;
}

std::optional<std::string> resolve_agent_workspace(const orangutan::AgentConfig &selected_agent) {
    try {
        auto workspace = orangutan::resolve_workspace_root(selected_agent.workspace);
        if (!workspace.empty()) {
            spdlog::info("Using workspace: {}", workspace);
        }
        return workspace;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        return std::nullopt;
    }
}

template <typename Store>
std::unique_ptr<Store> create_store(const char *name) {
    try {
        return std::make_unique<Store>();
    } catch (const std::exception &e) {
        std::cerr << "Error: failed to initialize " << name << ": " << e.what() << '\n';
        return nullptr;
    }
}

std::optional<std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig>> build_agent_runtime_configs(const orangutan::Config &cfg,
                                                                                                               const std::string &cli_api_key_override) {
    std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig> result;
    for (const auto &[agent_key, agent_cfg] : cfg.agents) {
        orangutan::Config agent_cfg_wrapper = cfg;
        agent_cfg_wrapper.api_key = agent_cfg.api_key;
        const auto resolved_agent_api_key = resolve_api_key(cli_api_key_override, agent_cfg_wrapper);
        if (resolved_agent_api_key.empty()) {
            std::cerr << "Error: API key required for agent '" << agent_key << "'.\n";
            return std::nullopt;
        }

        std::string resolved_workspace_root;
        try {
            resolved_workspace_root = orangutan::resolve_workspace_root(agent_cfg.workspace);
        } catch (const std::exception &e) {
            std::cerr << "Error: failed to resolve workspace for agent '" << agent_key << "': " << e.what() << '\n';
            return std::nullopt;
        }

        const auto cli_identity = orangutan::derive_cli_identity(resolved_workspace_root, agent_key);

        result.emplace(agent_key, orangutan::app::AgentRuntimeConfig{
                                      .agent_key = agent_key,
                                      .provider_name = agent_cfg.provider,
                                      .api_key = resolved_agent_api_key,
                                      .model = agent_cfg.model,
                                      .fallback_models = agent_cfg.fallback_models,
                                      .base_url = agent_cfg.base_url,
                                      .system_prompt = agent_cfg.system_prompt,
                                      .workspace_root = resolved_workspace_root,
                                      .cli_runtime_key = cli_identity.runtime_key,
                                      .cli_memory_scope = cli_identity.memory_scope,
                                      .memory = cfg.memory,
                                      .permissions = agent_cfg.permissions,
                                      .allowed_child_agents = agent_cfg.subagents,
                                  });
    }
    return result;
}

std::unordered_map<std::string, orangutan::SubagentChildRuntimeConfig>
build_subagent_child_runtime_configs(const std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig> &agent_runtime_configs) {
    std::unordered_map<std::string, orangutan::SubagentChildRuntimeConfig> result;
    for (const auto &[agent_key, runtime_cfg] : agent_runtime_configs) {
        result.emplace(agent_key, orangutan::SubagentChildRuntimeConfig{
                                      .agent_key = runtime_cfg.agent_key,
                                      .provider_name = runtime_cfg.provider_name,
                                      .api_key = runtime_cfg.api_key,
                                      .model = runtime_cfg.model,
                                      .fallback_models = runtime_cfg.fallback_models,
                                      .base_url = runtime_cfg.base_url,
                                      .system_prompt = runtime_cfg.system_prompt,
                                      .workspace_root = runtime_cfg.workspace_root,
                                      .memory = runtime_cfg.memory,
                                      .permissions = runtime_cfg.permissions,
                                      .allowed_child_agents = runtime_cfg.allowed_child_agents,
                                  });
    }
    return result;
}

std::unordered_map<std::string, std::string> build_qq_bot_agents(const orangutan::Config &cfg) {
    std::unordered_map<std::string, std::string> qq_bot_agents;
    for (const auto &bot : cfg.qq_bots) {
        if (!bot.name.empty()) {
            qq_bot_agents.insert_or_assign(bot.name, bot.agent);
        }
    }
    return qq_bot_agents;
}

bool choose_resume_session_id(const std::vector<orangutan::SessionInfo> &sessions, std::string &resume_session) {
    if (sessions.empty()) {
        std::cerr << "Error: no saved sessions to resume.\n"
                  << "Start a conversation first - sessions are auto-saved on exit.\n";
        return false;
    }

    if (resume_session == "latest" || sessions.size() == 1) {
        resume_session = sessions[0].id;
        return true;
    }

    if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
        resume_session = sessions[0].id;
        return true;
    }

    std::cout << "Available sessions:\n";
    for (size_t index = 0; index < sessions.size(); ++index) {
        const auto &session = sessions[index];
        std::cout << "  [" << (index + 1) << "] " << session.id << "  " << session.created_at << "  " << session.model << "  (" << session.message_count << " messages)\n";
    }
    std::cout << "\nEnter number (or press Enter for latest): " << std::flush;

    std::string choice;
    std::getline(std::cin, choice);
    if (choice.empty()) {
        resume_session = sessions[0].id;
        return true;
    }

    try {
        const auto idx = std::stoul(choice) - 1;
        if (idx >= sessions.size()) {
            std::cerr << "Invalid selection.\n";
            return false;
        }
        resume_session = sessions[idx].id;
        return true;
    } catch (const std::exception &) {
        std::cerr << "Invalid selection.\n";
        return false;
    }
}

bool restore_requested_session(const CliOptions &options, orangutan::SessionStore &session_store, const orangutan::app::AgentRuntimeConfig &runtime_cfg,
                               orangutan::AgentLoop &agent, std::string &resume_session, std::string &current_session_id) {
    if (!options.resume_requested) {
        return true;
    }

    if (resume_session.empty() || resume_session == "latest") {
        auto sessions = session_store.list_sessions(runtime_cfg.cli_memory_scope);
        if (!choose_resume_session_id(sessions, resume_session)) {
            return false;
        }
    }

    try {
        if (!runtime_cfg.cli_memory_scope.empty() && !session_store.session_belongs_to_scope(resume_session, runtime_cfg.cli_memory_scope)) {
            std::cerr << "Error: session does not belong to agent '" << options.cli_agent_key << "'.\n";
            return false;
        }
        auto messages = session_store.load(resume_session);
        agent.set_history(std::move(messages));
        current_session_id = resume_session;
        if (!options.event_stream) {
            std::cout << "Resumed session: " << resume_session << '\n';
        }
        return true;
    } catch (const std::exception &) {
        std::cerr << "Error: session not found: " << resume_session << '\n';
        auto sessions = session_store.list_sessions(runtime_cfg.cli_memory_scope);
        if (sessions.empty()) {
            std::cerr << "No saved sessions available.\n";
        } else {
            std::cerr << "Available sessions:\n";
            for (const auto &session : sessions) {
                std::cerr << "  " << session.id << "  " << session.created_at << "  " << session.model << "  (" << session.message_count << " messages)\n";
            }
        }
        return false;
    }
}

std::string merge_stdin_message(std::string message) {
    auto stdin_content = read_stdin_if_piped();
    if (stdin_content.empty()) {
        return message;
    }

    if (message.empty()) {
        return stdin_content;
    }
    return message + "\n\n" + stdin_content;
}

bool validate_runtime_mode_options(const CliOptions &options, bool has_current_session) {
    if (options.serve_mode && !options.message.empty()) {
        std::cerr << "Error: --serve cannot be combined with --message or piped stdin.\n";
        return false;
    }
    if (options.event_stream && options.message.empty() && !options.dump_session) {
        std::cerr << "Error: --event-stream requires --message or piped stdin.\n";
        return false;
    }
    if (!options.dump_session) {
        return true;
    }
    if (!options.event_stream) {
        std::cerr << "Error: --dump-session requires --event-stream.\n";
        return false;
    }
    if (!has_current_session) {
        std::cerr << "Error: --dump-session requires --resume.\n";
        return false;
    }
    return true;
}

int run_serve_mode(orangutan::ChannelManager &channel_manager, orangutan::MessageQueue &message_queue,
                   const std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig> &agent_runtime_configs,
                   const std::unordered_map<std::string, std::string> &qq_bot_agents, orangutan::MemoryStore *memory_store, orangutan::SessionStore &session_store,
                   orangutan::SubagentManager &subagent_manager, const orangutan::Config &cfg, orangutan::HookManager *hook_manager) {
    if (!channel_manager.has_channels() && cfg.heartbeat_jobs.empty()) {
        std::cerr << "Error: --serve requires at least one configured channel or heartbeat job.\n";
        return 1;
    }

    auto &stop_requested = signal_stop_requested();
    stop_requested.store(false);
    auto channel_task_runner = std::make_unique<orangutan::JidTaskRunner>(orangutan::app::default_serve_worker_count());
    spdlog::info("Starting channel executor (configured concurrency hint: {})", channel_task_runner->worker_count());

    static constexpr std::string_view heartbeat_protocol_suffix = "\n\n---\n"
                                                                  "HEARTBEAT PROTOCOL: If everything looks fine and nothing needs attention, "
                                                                  "respond with exactly \"HEARTBEAT_OK\" (optionally followed by a brief status note). "
                                                                  "This suppresses delivery and saves tokens. Only use HEARTBEAT_OK when there is truly nothing to report.";

    orangutan::HeartbeatScheduler heartbeat_scheduler([&message_queue, &cfg](const orangutan::HeartbeatJob &job) {
        auto jid = std::string("heartbeat:") + job.name;
        auto prompt = job.prompt + std::string(heartbeat_protocol_suffix);
        message_queue.push(orangutan::InboundMessage{
            .jid = jid,
            .content = std::move(prompt),
            .agent_override = job.agent,
            .reply_target = job.channel,
            .isolated = cfg.isolated_session,
            .light_context = cfg.light_context,
        });
    });

    heartbeat_scheduler.set_heartbeat_md_path(cfg.heartbeat_md_path);

    for (const auto &job_cfg : cfg.heartbeat_jobs) {
        auto expr = orangutan::parse_cron(job_cfg.cron);
        if (!expr.has_value()) {
            spdlog::error("Invalid cron expression for heartbeat job '{}': '{}'", job_cfg.name, job_cfg.cron);
            continue;
        }
        heartbeat_scheduler.add_job(job_cfg.name, *expr, job_cfg.agent, job_cfg.channel, job_cfg.prompt);
        auto next = orangutan::next_fire_time(*expr, std::chrono::system_clock::now());
        if (next.has_value()) {
            auto next_time = std::chrono::system_clock::to_time_t(*next);
            std::tm next_tm{};
            localtime_r(&next_time, &next_tm);
            std::array<char, 64> buf{};
            std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &next_tm);
            spdlog::info("Heartbeat job '{}' [{}] next fire: {}", job_cfg.name, job_cfg.cron, buf.data());
        }
    }

    const auto *const home = std::getenv("HOME");
    auto cron_store_path = home != nullptr ? std::string(home) + "/.orangutan/cron/jobs.json" : std::string{};
    orangutan::CronStore cron_store(cron_store_path);
    for (const auto &entry : cron_store.jobs()) {
        if (heartbeat_scheduler.has_job(entry.name)) {
            spdlog::warn("Skipping persisted cron job '{}' — conflicts with config job", entry.name);
            continue;
        }
        auto expr = orangutan::parse_cron(entry.cron);
        if (!expr.has_value()) {
            spdlog::warn("Invalid cron expression in persisted job '{}': '{}'", entry.name, entry.cron);
            continue;
        }
        heartbeat_scheduler.add_job(entry.name, *expr, entry.agent, entry.channel, entry.prompt, true);
        spdlog::info("Loaded persisted cron job '{}'", entry.name);
    }

    try {
        channel_manager.connect_all([&message_queue](const orangutan::InboundMessage &message) {
            message_queue.push(message);
        });
    } catch (const std::exception &e) {
        std::cerr << "Error: failed to start configured channels: " << e.what() << '\n';
        channel_manager.disconnect_all();
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    heartbeat_scheduler.start();

    orangutan::app::run_channel_loop(message_queue, channel_manager, stop_requested, *channel_task_runner, agent_runtime_configs, qq_bot_agents, memory_store, session_store,
                                     subagent_manager, cfg, hook_manager, &cron_store, &heartbeat_scheduler);

    heartbeat_scheduler.stop();
    channel_task_runner->shutdown(true);
    channel_manager.disconnect_all();
    return 0;
}

} // namespace

int orangutan::app::run_bootstrap(int argc, char **argv) {
    CliOptions options;
    CLI::App app{"orangutan - your local AI assistant"};
    CLI::Option *resume_flag = nullptr;
    configure_cli_app(app, options, resume_flag);
    CLI11_PARSE(app, argc, argv);
    options.resume_requested = resume_flag->count() > 0;

    configure_logging(options.verbose);
    auto cfg = orangutan::Config::load();
    if (!options.edit_mode.empty()) {
        cfg.edit_mode = options.edit_mode;
    }
    if (!validate_initial_options(options)) {
        return 1;
    }

    const auto maybe_selected_agent = resolve_selected_agent(cfg, options);
    if (!maybe_selected_agent.has_value()) {
        return 1;
    }

    const auto maybe_workspace = resolve_agent_workspace(*maybe_selected_agent);
    if (!maybe_workspace.has_value()) {
        return 1;
    }

    orangutan::Config selected_agent_cfg = cfg;
    selected_agent_cfg.api_key = maybe_selected_agent->api_key;
    const auto api_key = resolve_api_key(options.api_key, selected_agent_cfg);
    if (api_key.empty()) {
        std::cerr << "Error: API key required.\n"
                  << "Set ANTHROPIC_API_KEY, LLM_API_KEY, or use --api-key\n";
        return 1;
    }

    auto memory_store = create_store<orangutan::MemoryStore>("memory store");
    auto session_store = create_store<orangutan::SessionStore>("session store");
    auto subagent_run_store = create_store<orangutan::SubagentRunStore>("subagent run store");
    if (memory_store == nullptr || session_store == nullptr || subagent_run_store == nullptr) {
        return 1;
    }

    const auto maybe_agent_runtime_configs = build_agent_runtime_configs(cfg, options.api_key);
    if (!maybe_agent_runtime_configs.has_value()) {
        return 1;
    }
    const auto qq_bot_agents = build_qq_bot_agents(cfg);
    const auto subagent_child_runtime_configs = build_subagent_child_runtime_configs(*maybe_agent_runtime_configs);
    orangutan::SubagentManager subagent_manager(*subagent_run_store, orangutan::SubagentExecutionEnvironment{
                                                                         .agent_configs = &subagent_child_runtime_configs,
                                                                         .session_store = session_store.get(),
                                                                         .memory_store = memory_store.get(),
                                                                     });

    const auto cli_identity = orangutan::derive_cli_identity(*maybe_workspace, options.cli_agent_key);

    const auto runtime_cfg = orangutan::app::AgentRuntimeConfig{
        .agent_key = options.cli_agent_key,
        .provider_name = maybe_selected_agent->provider,
        .api_key = api_key,
        .model = maybe_selected_agent->model,
        .fallback_models = maybe_selected_agent->fallback_models,
        .base_url = maybe_selected_agent->base_url,
        .system_prompt = maybe_selected_agent->system_prompt,
        .workspace_root = *maybe_workspace,
        .cli_runtime_key = cli_identity.runtime_key,
        .cli_memory_scope = cli_identity.memory_scope,
        .memory = cfg.memory,
        .permissions = maybe_selected_agent->permissions,
        .allowed_child_agents = maybe_selected_agent->subagents,
    };

    std::string current_session_id;
    auto provider = orangutan::create_provider_with_fallbacks(runtime_cfg.provider_name, api_key, runtime_cfg.model, runtime_cfg.base_url, runtime_cfg.fallback_models);
    orangutan::ToolRegistry tools;
    auto tool_context = orangutan::ToolRuntimeContext{
        .runtime_key = cli_identity.runtime_key,
        .agent_key = runtime_cfg.agent_key,
        .scope_key = runtime_cfg.cli_memory_scope,
        .current_session_id = &current_session_id,
        .allowed_child_agents = runtime_cfg.allowed_child_agents,
        .is_child_run = false,
        .subagent_manager = &subagent_manager,
        .runtime_origin = orangutan::SubagentRuntimeOrigin::cli,
        .raw_caller_id = "cli:local",
    };
    orangutan::RuntimeMemory runtime_memory(*memory_store, orangutan::make_runtime_memory_context(cli_identity, runtime_cfg.memory));
    const auto approval_callback = make_cli_approval_callback(!options.event_stream && !options.serve_mode);
    tool_context.approval_callback = approval_callback;
    auto tool_bootstrap = orangutan::register_runtime_tools(tools, &runtime_memory, cli_identity.workspace, &tool_context, cfg.custom_tools, cfg.mcp_servers,
                                                            &runtime_cfg.permissions, approval_callback, cfg.edit_mode);
    (void)tool_bootstrap;
    const auto system_prompt = orangutan::append_subagent_prompt_guidance(runtime_cfg.system_prompt, runtime_cfg.allowed_child_agents, false);

    orangutan::SkillLoader skill_loader;
    auto skill_dirs = orangutan::resolve_skill_directories(cfg.skill_paths, runtime_cfg.workspace_root);
    skill_loader.load_from_directories(skill_dirs);
    if (!skill_loader.active_skills().empty()) {
        spdlog::info("Loaded {} skill(s)", skill_loader.active_skills().size());
    }

    orangutan::HookManager hook_manager;
    {
        std::vector<std::string> hook_dirs = cfg.hook_paths;
        if (hook_dirs.empty()) {
            const char *home = std::getenv("HOME");
            if (home != nullptr) {
                hook_dirs.push_back(std::string(home) + "/.orangutan/hooks");
            }
            if (!runtime_cfg.workspace_root.empty()) {
                hook_dirs.push_back(runtime_cfg.workspace_root + "/.orangutan/hooks");
            }
        }
        for (const auto &dir : hook_dirs) {
            spdlog::info("Hook directory: {}", dir);
        }
        hook_manager.load_from_directories(hook_dirs);
        for (const auto event : {orangutan::HookEvent::before_tool_call, orangutan::HookEvent::after_tool_call, orangutan::HookEvent::message_received,
                                 orangutan::HookEvent::message_sending, orangutan::HookEvent::session_start, orangutan::HookEvent::session_end}) {
            spdlog::info("Hook count for '{}': {}", orangutan::hook_event_to_string(event), hook_manager.hook_count(event));
        }
        spdlog::info("Loaded {} hook(s) total", hook_manager.total_hooks());
    }

    orangutan::AgentLoop agent(*provider, tools, system_prompt, &runtime_memory, skill_loader.build_prompt_section(), &hook_manager);

    orangutan::ChannelManager channel_manager(orangutan::Allowlist(cfg.allow, cfg.deny));
    orangutan::app::add_configured_channels(channel_manager, cfg);

    auto resume_session = options.resume_session;
    if (!restore_requested_session(options, *session_store, runtime_cfg, agent, resume_session, current_session_id)) {
        return 1;
    }

    options.message = merge_stdin_message(options.message);
    if (!validate_runtime_mode_options(options, !current_session_id.empty())) {
        return 1;
    }

    if (options.dump_session) {
        orangutan::app::emit_session_history_dump(agent.history(), current_session_id, emit_json_event);
        return 0;
    }

    if (options.serve_mode) {
        orangutan::MessageQueue message_queue;
        return run_serve_mode(channel_manager, message_queue, *maybe_agent_runtime_configs, qq_bot_agents, memory_store.get(), *session_store, subagent_manager, cfg,
                              &hook_manager);
    }

    if (!options.message.empty()) {
        return orangutan::app::run_single_message(agent, *provider, *session_store, cfg, options.message, options.event_stream, current_session_id, runtime_cfg.model,
                                                  runtime_cfg.cli_memory_scope, emit_json_event, std::cerr);
    }

    orangutan::app::run_repl(agent, *provider, *session_store, runtime_cfg.model, runtime_cfg.fallback_models, cfg, current_session_id, runtime_cfg.agent_key,
                             runtime_cfg.cli_memory_scope, &skill_loader, &tools, &hook_manager);
    return 0;
}
