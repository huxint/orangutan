#include "app/bootstrap.hpp"

#include "app/channel-serve.hpp"
#include "app/repl.hpp"
#include "app/session-workflow.hpp"
#include "app/single-shot.hpp"
#include "app/runtime/app-runtime.hpp"
#include "app/runtime/agent-runtime.hpp"
#include "app/runtime/identity.hpp"
#include "core/tools/tool.hpp"
#include "features/web/web-server.hpp"
#include "features/channel/core/message-queue.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/config/config.hpp"
#include "infra/config/secret-protection.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"

#include <CLI/CLI.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

std::atomic<bool> &signal_stop_requested();

} // namespace

namespace orangutan::app::detail {

struct WebStartupInspection {
    bool has_session_store = false;
    bool has_memory_store = false;
    bool has_subagent_run_store = false;
    bool has_subagent_manager = false;
    bool has_runtime_bundle = false;
    bool has_runtime_agent = false;
    bool attached_session_store = false;
    bool attached_tool_registry = false;
    bool attached_skill_loader = false;
    std::vector<ToolDef> tool_definitions;
    std::vector<std::string> active_skill_names;
    std::string runtime_build_error;
};

using WebStartupInspectionCallback = std::function<bool(const WebStartupInspection &)>;
using WebRuntimeBuildCallback = std::function<void()>;
using ChannelModeCallback = std::function<int()>;

WebStartupInspectionCallback &web_startup_inspection_callback() {
    static WebStartupInspectionCallback callback;
    return callback;
}

WebRuntimeBuildCallback &web_runtime_build_callback() {
    static WebRuntimeBuildCallback callback;
    return callback;
}

ChannelModeCallback &channel_mode_callback() {
    static ChannelModeCallback callback;
    return callback;
}

std::string resolve_api_key(const std::string &cli_api_key_override, const Config &cfg) {
    if (!cli_api_key_override.empty()) {
        return cli_api_key_override;
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

void reset_signal_stop_requested_for_tests() {
    signal_stop_requested().store(false);
}

void set_web_startup_inspection_callback_for_tests(WebStartupInspectionCallback callback) {
    web_startup_inspection_callback() = std::move(callback);
}

void clear_web_startup_inspection_callback_for_tests() {
    web_startup_inspection_callback() = {};
}

void set_web_runtime_build_callback_for_tests(WebRuntimeBuildCallback callback) {
    web_runtime_build_callback() = std::move(callback);
}

void clear_web_runtime_build_callback_for_tests() {
    web_runtime_build_callback() = {};
}

void set_channel_mode_callback_for_tests(ChannelModeCallback callback) {
    channel_mode_callback() = std::move(callback);
}

void clear_channel_mode_callback_for_tests() {
    channel_mode_callback() = {};
}

void maybe_inject_web_runtime_build_failure_for_tests() {
    auto &callback = web_runtime_build_callback();
    if (callback != nullptr) {
        callback();
    }
}

bool inspect_web_startup_for_tests(const WebStartupInspection &inspection) {
    auto &callback = web_startup_inspection_callback();
    if (callback == nullptr) {
        return false;
    }
    return callback(inspection);
}

} // namespace orangutan::app::detail

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

std::string default_workspace_hint() {
    const char *home = std::getenv("HOME");
    if (home == nullptr || std::string_view{home}.empty()) {
        return {};
    }
    return (std::filesystem::path(home) / ".orangutan" / "workspace" / "main").lexically_normal().string();
}

void emit_json_event(const orangutan::json &event) {
    spdlog::fmt_lib::println("{}", event.dump());
    std::fflush(stdout);
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

std::atomic<bool> &signal_stop_requested() {
    static std::atomic<bool> stop_requested{false};
    return stop_requested;
}

void handle_signal(int /*signum*/) {
    signal_stop_requested().store(true);
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
                   "Rewrite supported plaintext config secrets in place and exit. Optional argument: config path; defaults to ~/.orangutan/config.toml")
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

orangutan::ToolApprovalCallback make_cli_approval_callback(bool allow_prompting) {
    if (!allow_prompting || isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
        return {};
    }

    return [](const orangutan::ToolUseBlock &, const std::string &prompt_text) {
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

void load_display_skills(orangutan::SkillLoader &skill_loader, const orangutan::Config &cfg, const std::string &workspace_root) {
    auto skill_dirs = orangutan::resolve_skill_directories(cfg.skill_paths, workspace_root);
    skill_loader.load_from_directories(skill_dirs);
    if (!skill_loader.active_skills().empty()) {
        spdlog::info("Loaded {} skill(s)", skill_loader.active_skills().size());
    }
}

std::vector<std::string> resolve_runtime_hook_dirs(const orangutan::Config &cfg, const std::string &workspace_root) {
    std::vector<std::string> hook_dirs = cfg.hook_paths;
    if (!hook_dirs.empty()) {
        return hook_dirs;
    }

    if (const char *home = std::getenv("HOME"); home != nullptr) {
        hook_dirs.push_back(std::string(home) + "/.orangutan/hooks");
    }
    if (!workspace_root.empty()) {
        hook_dirs.push_back(workspace_root + "/.orangutan/hooks");
    }
    return hook_dirs;
}

void log_loaded_hooks(const std::vector<std::string> &hook_dirs, const orangutan::HookManager &hook_manager) {
    for (const auto &dir : hook_dirs) {
        spdlog::info("Hook directory: {}", dir);
    }
    for (const auto event : {orangutan::HookEvent::before_tool_call, orangutan::HookEvent::after_tool_call, orangutan::HookEvent::message_received,
                             orangutan::HookEvent::message_sending, orangutan::HookEvent::session_start, orangutan::HookEvent::session_end}) {
        spdlog::info("Hook count for '{}': {}", magic_enum::enum_name(event), hook_manager.hook_count(event));
    }
    spdlog::info("Loaded {} hook(s) total", hook_manager.total_hooks());
}

struct WebServerRuntimeAttachments {
    bool session_store_attached = false;
    bool tool_registry_attached = false;
    bool skill_loader_attached = false;
};

std::vector<std::string> collect_active_skill_names(const orangutan::SkillLoader *skill_loader) {
    if (skill_loader == nullptr) {
        return {};
    }

    std::vector<std::string> skill_names;
    skill_names.reserve(skill_loader->active_skills().size());
    for (const auto &skill : skill_loader->active_skills()) {
        skill_names.push_back(skill.name);
    }
    return skill_names;
}

orangutan::app::detail::WebStartupInspection build_web_startup_inspection(orangutan::SessionStore *session_store, orangutan::MemoryStore *memory_store,
                                                                          orangutan::SubagentRunStore *subagent_run_store, orangutan::SubagentManager *subagent_manager,
                                                                          const orangutan::AgentRuntimeBundle *runtime, orangutan::SkillLoader *skill_loader,
                                                                          const WebServerRuntimeAttachments &attachments, std::string_view runtime_build_error) {
    orangutan::app::detail::WebStartupInspection inspection;
    inspection.has_session_store = session_store != nullptr;
    inspection.has_memory_store = memory_store != nullptr;
    inspection.has_subagent_run_store = subagent_run_store != nullptr;
    inspection.has_subagent_manager = subagent_manager != nullptr;
    inspection.has_runtime_bundle = runtime != nullptr;
    inspection.has_runtime_agent = runtime != nullptr && runtime->agent != nullptr;
    inspection.attached_session_store = attachments.session_store_attached;
    inspection.attached_tool_registry = attachments.tool_registry_attached;
    inspection.attached_skill_loader = attachments.skill_loader_attached;
    if (runtime != nullptr) {
        inspection.tool_definitions = runtime->tools.definitions();
    }
    inspection.active_skill_names = collect_active_skill_names(skill_loader);
    inspection.runtime_build_error = std::string(runtime_build_error);
    return inspection;
}

bool maybe_skip_web_server_start_for_tests(orangutan::SessionStore *session_store, orangutan::MemoryStore *memory_store, orangutan::SubagentRunStore *subagent_run_store,
                                           orangutan::SubagentManager *subagent_manager, const orangutan::AgentRuntimeBundle *runtime, orangutan::SkillLoader *skill_loader,
                                           const WebServerRuntimeAttachments &attachments, std::string_view runtime_build_error = {}) {
    return orangutan::app::detail::inspect_web_startup_for_tests(
        build_web_startup_inspection(session_store, memory_store, subagent_run_store, subagent_manager, runtime, skill_loader, attachments, runtime_build_error));
}

WebServerRuntimeAttachments configure_web_server_runtime(orangutan::WebServer &web_server, const CliOptions &options, orangutan::Config &cfg,
                                                         orangutan::SessionStore *session_store, orangutan::MemoryStore *memory_store, orangutan::SubagentManager *subagent_manager,
                                                         orangutan::automation::Runtime *automation_runtime = nullptr, orangutan::ToolRegistry *tool_registry = nullptr,
                                                         orangutan::SkillLoader *skill_loader = nullptr) {
    WebServerRuntimeAttachments attachments;
    web_server.set_static_dir(options.web_dir);
    web_server.set_config(&cfg);
    if (session_store != nullptr) {
        web_server.set_session_store(session_store);
        attachments.session_store_attached = true;
    }
    web_server.set_memory_store(memory_store);
    web_server.set_subagent_manager(subagent_manager);
    web_server.set_automation_runtime(automation_runtime);
    if (tool_registry != nullptr) {
        web_server.set_tool_registry(tool_registry);
        attachments.tool_registry_attached = true;
    }
    if (skill_loader != nullptr) {
        web_server.set_skill_loader(skill_loader);
        attachments.skill_loader_attached = true;
    }
    return attachments;
}

void warn_if_nonlocal_web_host(const std::string &web_host) {
    if (web_host != "127.0.0.1") {
        spdlog::warn("Web server binding to {} — accessible from network", web_host);
    }
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
    const auto path = options.protect_config_path.empty() ? orangutan::default_orangutan_config_path() : std::filesystem::path{options.protect_config_path};
    if (path.empty()) {
        spdlog::fmt_lib::println(stderr, "Error: could not resolve the default config path.");
        return 1;
    }
    if (!std::filesystem::exists(path)) {
        spdlog::fmt_lib::println(stderr, "Error: config file not found: {}", path.string());
        return 1;
    }

    try {
        orangutan::ConfigSecretOptions secret_options{
            .password_override = options.config_password,
            .allow_interactive_password = true,
        };
        const auto password = orangutan::resolve_config_secret_password(secret_options);
        const auto result = orangutan::protect_config_file_secrets(path, password);
        if (!result.modified) {
            spdlog::fmt_lib::println("No eligible plaintext config secrets found in {}.", path.string());
            return 0;
        }

        static_cast<void>(orangutan::Config::load_from(path, orangutan::ConfigSecretOptions{
                                                                 .password_override = password,
                                                             }));

        spdlog::fmt_lib::println("Protected {} config secret(s) in {}", result.protected_count, path.string());
        spdlog::fmt_lib::println("Backup written to {}", result.backup_path.string());
        return 0;
    } catch (const orangutan::ConfigSecretProtectionError &e) {
        spdlog::fmt_lib::println(stderr, "Error: {}", e.what());
        return 1;
    }
}

std::optional<orangutan::AgentConfig> resolve_selected_agent(const orangutan::Config &cfg, const CliOptions &options) {
    const auto effective_agents = orangutan::app::detail::build_effective_agents(cfg);
    const auto agent_it = effective_agents.find(options.cli_agent_key);
    if (agent_it == effective_agents.end()) {
        spdlog::fmt_lib::println(stderr, "Error: unknown agent: {}", options.cli_agent_key);
        return std::nullopt;
    }

    auto selected_agent = agent_it->second;
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
        spdlog::fmt_lib::println(stderr, "Error: {}", e.what());
        return std::nullopt;
    }
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

struct RuntimeCompletionResumeState {
    std::mutex mutex;
    orangutan::AgentLoop *agent = nullptr;
    orangutan::Provider *provider = nullptr;
    orangutan::HookManager *hook_manager = nullptr;
    orangutan::SessionStore *session_store = nullptr;
    std::string *current_session_id = nullptr;
    std::string agent_key;
    std::string configured_model;
    std::string scope_key;
    orangutan::automation::Runtime *automation_runtime = nullptr;
    bool persist_session = false;
    bool suppress_human_output = false;
};

void deactivate_runtime_completion_resume_state(const std::shared_ptr<RuntimeCompletionResumeState> &state) {
    if (state == nullptr) {
        return;
    }

    std::scoped_lock lock(state->mutex);
    state->agent = nullptr;
    state->provider = nullptr;
    state->hook_manager = nullptr;
    state->session_store = nullptr;
    state->current_session_id = nullptr;
    state->automation_runtime = nullptr;
}

struct RuntimeCompletionResumeStateGuard {
    std::shared_ptr<RuntimeCompletionResumeState> state;

    explicit RuntimeCompletionResumeStateGuard(std::shared_ptr<RuntimeCompletionResumeState> state_ptr)
    : state(std::move(state_ptr)) {}

    RuntimeCompletionResumeStateGuard(const RuntimeCompletionResumeStateGuard &) = delete;
    RuntimeCompletionResumeStateGuard &operator=(const RuntimeCompletionResumeStateGuard &) = delete;
    RuntimeCompletionResumeStateGuard(RuntimeCompletionResumeStateGuard &&) = delete;
    RuntimeCompletionResumeStateGuard &operator=(RuntimeCompletionResumeStateGuard &&) = delete;

    ~RuntimeCompletionResumeStateGuard() {
        deactivate_runtime_completion_resume_state(state);
    }
};

orangutan::BackgroundCompletionResumeCallback make_runtime_completion_resume_callback(const std::weak_ptr<RuntimeCompletionResumeState> &weak_state) {
    return [weak_state](const std::string &message) -> std::optional<std::string> {
        const auto state = weak_state.lock();
        if (!state) {
            return "runtime is no longer available";
        }

        std::scoped_lock lock(state->mutex);
        if (state->agent == nullptr) {
            return "runtime is no longer available";
        }

        return orangutan::app::run_completion_resume_message(
            *state->agent, message, state->agent_key, state->automation_runtime,
            [state](const std::string &) -> std::optional<std::string> {
                if (!state->persist_session || state->session_store == nullptr || state->current_session_id == nullptr || state->agent == nullptr) {
                    return std::nullopt;
                }

                const bool created_session = state->current_session_id->empty();
                const auto active_model = state->provider != nullptr && !state->provider->current_model().empty() ? state->provider->current_model() : state->configured_model;
                if (!orangutan::app::persist_session(*state->agent, *state->session_store, *state->current_session_id,
                                                     orangutan::app::make_cli_session_metadata(active_model, state->scope_key, state->agent_key))) {
                    return std::nullopt;
                }

                if (created_session) {
                    orangutan::dispatch_session_start(state->hook_manager, *state->current_session_id, state->agent->history().size());
                }
                return std::nullopt;
            },
            state->suppress_human_output);
    };
}

std::shared_ptr<const orangutan::BackgroundCompletionRuntimeBindings> make_runtime_background_completion_bindings(orangutan::automation::Runtime *automation_runtime,
                                                                                                                  orangutan::BackgroundCompletionResumeCallback resume_callback) {
    if (automation_runtime == nullptr) {
        return nullptr;
    }

    return orangutan::make_background_completion_runtime_bindings(
        [automation_runtime](const orangutan::automation::InboxItem &item) {
            static_cast<void>(automation_runtime->store().insert_inbox(item));
        },
        std::move(resume_callback));
}

} // namespace

namespace orangutan::app::detail {

std::unordered_map<std::string, AgentConfig> build_effective_agents(const orangutan::Config &cfg) {
    auto effective_agents = cfg.agents;
    if (!effective_agents.contains("default")) {
        effective_agents.insert_or_assign("default", orangutan::AgentConfig{
                                                         .provider = cfg.provider,
                                                         .model = cfg.model,
                                                         .fallback_models = cfg.fallback_models,
                                                         .base_url = cfg.base_url,
                                                         .api_key = cfg.api_key,
                                                         .system_prompt = cfg.system_prompt,
                                                         .workspace = cfg.workspace,
                                                         .permissions = cfg.permissions,
                                                         .subagents = {},
                                                         .edit_mode = cfg.edit_mode,
                                                         .thinking_budget = cfg.thinking_budget,
                                                     });
    }
    for (auto &[agent_key, agent_cfg] : effective_agents) {
        if (agent_cfg.workspace.empty()) {
            agent_cfg.workspace = default_workspace_hint();
        }
        static_cast<void>(agent_key);
    }
    return effective_agents;
}

std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const orangutan::Config &cfg, const std::string &cli_api_key_override) {
    std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig> result;
    for (const auto &[agent_key, agent_cfg] : build_effective_agents(cfg)) {
        orangutan::Config agent_cfg_wrapper = cfg;
        agent_cfg_wrapper.api_key = agent_cfg.api_key;
        const auto resolved_agent_api_key = orangutan::app::detail::resolve_api_key(cli_api_key_override, agent_cfg_wrapper);

        std::string resolved_workspace_root;
        try {
            resolved_workspace_root = orangutan::resolve_workspace_root(agent_cfg.workspace);
        } catch (const std::exception &e) {
            spdlog::fmt_lib::println(stderr, "Error: failed to resolve workspace for agent '{}': {}", agent_key, e.what());
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
                                      .edit_mode = agent_cfg.edit_mode,
                                      .thinking_budget = agent_cfg.thinking_budget,
                                      .cli_runtime_key = cli_identity.runtime_key,
                                      .cli_memory_scope = cli_identity.memory_scope,
                                      .memory = cfg.memory,
                                      .permissions = agent_cfg.permissions,
                                      .allowed_child_agents = agent_cfg.subagents,
                                  });
    }
    return result;
}

std::unordered_map<std::string, SubagentChildRuntimeConfig> build_subagent_child_runtime_configs(const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs) {
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
                                      .edit_mode = runtime_cfg.edit_mode,
                                      .thinking_budget = runtime_cfg.thinking_budget,
                                      .memory = runtime_cfg.memory,
                                      .permissions = runtime_cfg.permissions,
                                      .allowed_child_agents = runtime_cfg.allowed_child_agents,
                                  });
    }
    return result;
}

} // namespace orangutan::app::detail

namespace {

void apply_cli_edit_mode_override(orangutan::Config &cfg, std::string_view edit_mode) {
    if (edit_mode.empty()) {
        return;
    }

    cfg.edit_mode = std::string(edit_mode);
    for (auto &[agent_key, agent_cfg] : cfg.agents) {
        agent_cfg.edit_mode = cfg.edit_mode;
        static_cast<void>(agent_key);
    }
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
        spdlog::fmt_lib::println(stderr, "Error: no saved sessions to resume.");
        spdlog::fmt_lib::println(stderr, "Start a conversation first - sessions are auto-saved on exit.");
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

    spdlog::fmt_lib::println("Available sessions:");
    for (size_t index = 0; index < sessions.size(); ++index) {
        const auto &session = sessions[index];
        spdlog::fmt_lib::println("  [{}] {}  {}  {}  ({} messages)", index + 1, session.id, session.created_at, session.model, session.message_count);
    }
    spdlog::fmt_lib::print("\nEnter number (or press Enter for latest): ");
    std::fflush(stdout);

    std::string choice;
    std::getline(std::cin, choice);
    if (choice.empty()) {
        resume_session = sessions[0].id;
        return true;
    }

    try {
        const auto idx = std::stoul(choice) - 1;
        if (idx >= sessions.size()) {
            spdlog::fmt_lib::println(stderr, "Invalid selection.");
            return false;
        }
        resume_session = sessions[idx].id;
        return true;
    } catch (const std::exception &) {
        spdlog::fmt_lib::println(stderr, "Invalid selection.");
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
            spdlog::fmt_lib::println(stderr, "Error: session does not belong to agent '{}'.", options.cli_agent_key);
            return false;
        }
        auto messages = session_store.load(resume_session);
        agent.set_history(std::move(messages));
        current_session_id = resume_session;
        if (!options.event_stream) {
            spdlog::fmt_lib::println("Resumed session: {}", resume_session);
        }
        return true;
    } catch (const std::exception &) {
        spdlog::fmt_lib::println(stderr, "Error: session not found: {}", resume_session);
        auto sessions = session_store.list_sessions(runtime_cfg.cli_memory_scope);
        if (sessions.empty()) {
            spdlog::fmt_lib::println(stderr, "No saved sessions available.");
        } else {
            spdlog::fmt_lib::println(stderr, "Available sessions:");
            for (const auto &session : sessions) {
                spdlog::fmt_lib::println(stderr, "  {}  {}  {}  ({} messages)", session.id, session.created_at, session.model, session.message_count);
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
    if (options.event_stream && options.message.empty() && !options.dump_session) {
        spdlog::fmt_lib::println(stderr, "Error: --event-stream requires --message or piped stdin.");
        return false;
    }
    if (!options.dump_session) {
        return true;
    }
    if (!options.event_stream) {
        spdlog::fmt_lib::println(stderr, "Error: --dump-session requires --event-stream.");
        return false;
    }
    if (!has_current_session) {
        spdlog::fmt_lib::println(stderr, "Error: --dump-session requires --resume.");
        return false;
    }
    return true;
}

int run_channel_mode(orangutan::ChannelManager &channel_manager, orangutan::MessageQueue &message_queue,
                     const std::unordered_map<std::string, orangutan::app::AgentRuntimeConfig> &agent_runtime_configs,
                     const std::unordered_map<std::string, std::string> &qq_bot_agents, orangutan::MemoryStore *memory_store, orangutan::SessionStore &session_store,
                     orangutan::SubagentManager &subagent_manager, const orangutan::Config &cfg, orangutan::HookManager *hook_manager,
                     orangutan::automation::Runtime *automation_runtime) {
    auto &stop_requested = signal_stop_requested();
    stop_requested.store(false);
    auto channel_task_runner = std::make_unique<orangutan::JidTaskRunner>(orangutan::app::default_serve_worker_count());
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

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    orangutan::app::run_channel_loop(message_queue, channel_manager, stop_requested, *channel_task_runner, agent_runtime_configs, qq_bot_agents, memory_store, session_store,
                                     subagent_manager, cfg, hook_manager, automation_runtime);

    channel_task_runner->shutdown(true);
    channel_manager.disconnect_all();
    return 0;
}

} // namespace

int orangutan::app::run_bootstrap(int argc, char **argv) {
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
    std::optional<orangutan::app::AgentRuntimeConfig> maybe_primary_runtime_cfg;
    std::optional<orangutan::RuntimeIdentity> maybe_primary_identity;
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

        orangutan::Config selected_agent_cfg = cfg;
        selected_agent_cfg.api_key = maybe_selected_agent->api_key;
        primary_api_key = orangutan::app::detail::resolve_api_key(options.api_key, selected_agent_cfg);
        maybe_primary_identity = orangutan::derive_cli_identity(*maybe_workspace, options.cli_agent_key);
        maybe_primary_runtime_cfg = orangutan::app::AgentRuntimeConfig{
            .agent_key = options.cli_agent_key,
            .provider_name = maybe_selected_agent->provider,
            .api_key = primary_api_key,
            .model = maybe_selected_agent->model,
            .fallback_models = maybe_selected_agent->fallback_models,
            .base_url = maybe_selected_agent->base_url,
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
        spdlog::fmt_lib::println(stderr, "Set agent.api_key, ANTHROPIC_API_KEY, LLM_API_KEY, or use --api-key");
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
    orangutan::app::AppRuntime app_runtime;

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
            orangutan::RuntimeIdentity identity{
                .workspace = runtime_cfg.workspace_root,
                .runtime_key = "agent:" + runtime_cfg.agent_key + "|automation:" + trigger.automation_id,
                .memory_scope = "agent:" + runtime_cfg.agent_key + "|automation",
            };

            try {
                auto runtime = orangutan::build_agent_runtime(orangutan::AgentRuntimeBuildInput{
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
                    .runtime_origin = orangutan::SubagentRuntimeOrigin::cli,
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
    orangutan::app::add_configured_channels(channel_manager, cfg);
    app_runtime.automation_runtime().set_notifier([&channel_manager](std::string_view target, std::string_view message) -> std::optional<std::string> {
        try {
            channel_manager.send(std::string(target), std::string(message));
            return std::nullopt;
        } catch (const std::exception &e) {
            return e.what();
        }
    });
    app_runtime.automation_runtime().start();

    std::unique_ptr<orangutan::AgentRuntimeBundle> primary_runtime;
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
            orangutan::app::detail::maybe_inject_web_runtime_build_failure_for_tests();
            const auto approval_callback = make_cli_approval_callback(!options.event_stream);
            primary_runtime = std::make_unique<orangutan::AgentRuntimeBundle>(orangutan::build_agent_runtime(orangutan::AgentRuntimeBuildInput{
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
                .runtime_origin = orangutan::SubagentRuntimeOrigin::cli,
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
                    orangutan::app::emit_session_history_dump(primary_runtime->agent->history(), current_session_id, emit_json_event);
                } else if (!options.message.empty()) {
                    exit_code = orangutan::app::run_single_message(*primary_runtime->agent, *primary_runtime->provider, *session_store, cfg, options.message, options.event_stream,
                                                                   current_session_id, maybe_primary_runtime_cfg->model, maybe_primary_runtime_cfg->cli_memory_scope,
                                                                   maybe_primary_runtime_cfg->agent_key, emit_json_event, std::cerr, &app_runtime.automation_runtime());
                } else {
                    orangutan::app::run_repl(*primary_runtime->agent, *primary_runtime->provider, *session_store, maybe_primary_runtime_cfg->model,
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
