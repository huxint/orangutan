#include "app/channel-serve.hpp"

#include "features/agent/agent-loop.hpp"
#include "features/memory/runtime-memory.hpp"
#include "app/cli-ui.hpp"
#include "app/session-workflow.hpp"
#include "features/channel/platforms/qq.hpp"
#include "features/heartbeat/scheduler.hpp"
#include "features/heartbeat/protocol/heartbeat-ok.hpp"
#include "features/hooks/hook-manager.hpp"
#include "core/providers/provider.hpp"
#include "app/runtime/memory-context.hpp"
#include "app/runtime/identity.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/tools/runtime/runtime-loader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace orangutan::app {

namespace {

constexpr auto serve_poll_interval = std::chrono::milliseconds(50);

enum class ChannelApprovalDecision {
    approve,
    deny,
    invalid,
};

struct ParsedChannelApprovalReply {
    std::string request_id;
    ChannelApprovalDecision decision = ChannelApprovalDecision::invalid;
};

struct ConversationRuntime {
    std::unique_ptr<Provider> provider;
    ToolRegistry tools;
    std::unique_ptr<McpManager> mcp_manager;
    std::unique_ptr<RuntimeMemory> memory;
    std::unique_ptr<AgentLoop> agent;
    HookManager *hook_manager = nullptr;
    ToolRuntimeContext tool_context;
    std::string runtime_key;
    std::string agent_key;
    std::string model;
    std::string workspace;
    std::string memory_scope;
    std::string session_scope_key;
    std::string current_session_id;
    size_t persisted_message_count = 0;
};

std::string extract_qq_bot_name(const std::string &jid) {
    constexpr std::string_view prefix = "qqbot:";
    if (!jid.starts_with(prefix)) {
        return {};
    }

    const auto remainder = jid.substr(prefix.size());
    const auto first_colon = remainder.find(':');
    if (first_colon == std::string::npos) {
        return {};
    }

    auto first_segment = remainder.substr(0, first_colon);
    if (first_segment == "c2c" || first_segment == "group") {
        return {};
    }

    return {first_segment};
}

std::string normalize_channel_approval_token(std::string_view content) {
    std::string normalized;
    normalized.reserve(content.size());
    for (const auto ch : content) {
        const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (std::isalnum(static_cast<unsigned char>(lowered)) != 0 || lowered == '-') {
            normalized.push_back(lowered);
        }
    }
    return normalized;
}

ChannelApprovalDecision parse_channel_approval_decision(std::string_view content) {
    const auto normalized = normalize_channel_approval_token(content);
    if (normalized.empty()) {
        return ChannelApprovalDecision::invalid;
    }

    if (normalized == "y" || normalized == "yes" || normalized == "approve" || normalized == "approved" || normalized == "allow") {
        return ChannelApprovalDecision::approve;
    }
    if (normalized == "n" || normalized == "no" || normalized == "deny" || normalized == "denied" || normalized == "reject") {
        return ChannelApprovalDecision::deny;
    }
    return ChannelApprovalDecision::invalid;
}

ParsedChannelApprovalReply parse_channel_approval_reply(const std::string &content) {
    ParsedChannelApprovalReply parsed;
    std::istringstream stream(content);
    std::string token;
    while (stream >> token) {
        const auto normalized = normalize_channel_approval_token(token);
        if (normalized.starts_with("shell-approval-")) {
            parsed.request_id = normalized;
            continue;
        }

        const auto decision = parse_channel_approval_decision(normalized);
        if (decision != ChannelApprovalDecision::invalid) {
            parsed.decision = decision;
        }
    }
    return parsed;
}

std::string format_pending_channel_approval_prompt(const std::vector<std::string> &request_ids) {
    if (request_ids.empty()) {
        return "Shell approval is pending.";
    }

    if (request_ids.size() == 1) {
        return "Shell approval is pending. Reply with `" + request_ids.front() + " yes` or `" + request_ids.front() + " no`.";
    }

    std::ostringstream prompt;
    prompt << "Multiple shell approvals are pending. Reply with `<request-id> yes` or `<request-id> no`. Pending:";
    for (const auto &request_id : request_ids) {
        prompt << " " << request_id;
    }
    return prompt.str();
}

std::vector<std::string> pending_request_ids_for_jid(const std::unordered_map<std::string, std::vector<std::string>> &pending_request_ids_by_jid,
                                                     const std::string &jid) {
    const auto it = pending_request_ids_by_jid.find(jid);
    if (it == pending_request_ids_by_jid.end()) {
        return {};
    }
    return it->second;
}

bool can_prompt_for_channel_approval(const InboundMessage &message) {
    if (message.jid.starts_with("heartbeat:")) {
        return false;
    }

    const auto target = resolve_reply_target(message);
    if (target == "cli") {
        return false;
    }

    return target == message.jid;
}

ChannelApprovalDecision parse_channel_approval_decision(const std::string &content) {
    std::string normalized;
    normalized.reserve(content.size());
    for (const auto ch : content) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }

    return parse_channel_approval_decision(std::string_view{normalized});
}

std::unique_ptr<ConversationRuntime> make_conversation_runtime(const AgentRuntimeConfig &cfg, MemoryStore *memory_store, const RuntimeIdentity &identity,
                                                               SubagentManager &subagent_manager, const std::string &raw_caller_id, const std::string &skills_prompt,
                                                               const std::vector<Config::ScriptToolConfig> &custom_tools, const std::vector<Config::McpServerConfig> &mcp_servers,
                                                               HookManager *hook_manager, CronStore *cron_store, HeartbeatScheduler *heartbeat_scheduler) {
    auto runtime = std::make_unique<ConversationRuntime>();
    runtime->runtime_key = identity.runtime_key;
    runtime->agent_key = cfg.agent_key;
    runtime->model = cfg.model;
    runtime->workspace = identity.workspace;
    runtime->memory_scope = identity.memory_scope;
    runtime->session_scope_key = identity.runtime_key.empty() ? cfg.cli_runtime_key : identity.runtime_key;
    runtime->hook_manager = hook_manager;
    runtime->tool_context = ToolRuntimeContext{
        .runtime_key = identity.runtime_key,
        .agent_key = cfg.agent_key,
        .scope_key = identity.memory_scope,
        .current_session_id = &runtime->current_session_id,
        .allowed_child_agents = cfg.allowed_child_agents,
        .is_child_run = false,
        .subagent_manager = &subagent_manager,
        .runtime_origin = SubagentRuntimeOrigin::channel,
        .raw_caller_id = raw_caller_id,
        .cron_store = cron_store,
        .heartbeat_scheduler = heartbeat_scheduler,
    };
    runtime->provider = create_provider(cfg.provider_name, cfg.api_key, cfg.model, cfg.base_url);
    if (memory_store != nullptr) {
        runtime->memory = std::make_unique<RuntimeMemory>(*memory_store, make_runtime_memory_context(identity, cfg.memory));
    }
    auto tool_bootstrap =
        register_runtime_tools(runtime->tools, runtime->memory.get(), runtime->workspace, &runtime->tool_context, custom_tools, mcp_servers, &cfg.permissions);
    runtime->mcp_manager = std::move(tool_bootstrap.mcp_manager);
    auto system_prompt = append_subagent_prompt_guidance(cfg.system_prompt, cfg.allowed_child_agents, false);
    runtime->agent = std::make_unique<AgentLoop>(*runtime->provider, runtime->tools, system_prompt, runtime->memory.get(), skills_prompt, hook_manager);
    return runtime;
}

void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store) {
    const auto &history = runtime.agent->history();
    if (history.empty()) {
        session_store.clear_jid(jid, runtime.agent_key);
        runtime.current_session_id.clear();
        runtime.persisted_message_count = 0;
        return;
    }

    const bool created_session = runtime.current_session_id.empty();
    if (runtime.current_session_id.empty()) {
        runtime.current_session_id = session_store.save(history, runtime.model, runtime.session_scope_key);
        runtime.persisted_message_count = history.size();
    } else if (history.size() > runtime.persisted_message_count) {
        session_store.append(runtime.current_session_id, history, runtime.persisted_message_count);
        runtime.persisted_message_count = history.size();
    } else {
        session_store.update(runtime.current_session_id, history);
        runtime.persisted_message_count = history.size();
    }

    session_store.bind_jid(jid, runtime.current_session_id, runtime.agent_key);
    if (created_session) {
        dispatch_session_start(runtime.hook_manager, runtime.current_session_id, history.size());
    }
}

ConversationRuntime &ensure_runtime_for_jid(const std::string &jid, std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes, std::mutex &runtimes_mutex,
                                            const InboundMessage &message, const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs,
                                            const std::unordered_map<std::string, std::string> &qq_bot_agents, MemoryStore *memory_store, SessionStore &session_store,
                                            SubagentManager &subagent_manager, const Config &cfg, HookManager *hook_manager, CronStore *cron_store,
                                            HeartbeatScheduler *heartbeat_scheduler) {
    std::scoped_lock lock(runtimes_mutex);
    const auto agent_key = resolve_agent_key_for_message(message, qq_bot_agents);
    const auto runtime_key = derive_channel_runtime_key(jid, agent_key);
    auto it = runtimes.find(runtime_key);
    if (it == runtimes.end()) {
        const auto cfg_it = agent_configs.find(agent_key);
        if (cfg_it == agent_configs.end()) {
            throw std::runtime_error("No runtime configuration for agent: " + agent_key);
        }

        auto identity = derive_channel_identity(cfg_it->second.workspace_root, jid, agent_key);
        auto skill_prompt = build_skill_prompt_for_runtime(cfg, cfg_it->second);
        auto runtime = make_conversation_runtime(cfg_it->second, memory_store, identity, subagent_manager, jid, skill_prompt, cfg.custom_tools, cfg.mcp_servers, hook_manager,
                                                 cron_store, heartbeat_scheduler);
        if (!message.isolated) {
            if (auto session_id = session_store.bound_session_for_jid(jid, agent_key); session_id.has_value()) {
                try {
                    if (!session_store.session_belongs_to_scope(*session_id, runtime->session_scope_key)) {
                        spdlog::warn("Session {} does not belong to runtime scope '{}' for jid '{}' agent '{}'", *session_id, runtime->session_scope_key, jid, agent_key);
                        session_store.clear_jid(jid, agent_key);
                    } else {
                        runtime->agent->set_history(session_store.load(*session_id));
                        runtime->current_session_id = *session_id;
                        runtime->persisted_message_count = runtime->agent->history().size();
                        spdlog::info("Restored session {} for jid '{}' agent '{}'", *session_id, jid, agent_key);
                        dispatch_session_start(runtime->hook_manager, runtime->current_session_id, runtime->agent->history().size());
                    }
                } catch (const std::exception &e) {
                    spdlog::warn("Failed to restore session {} for jid '{}' agent '{}': {}", *session_id, jid, agent_key, e.what());
                    session_store.clear_jid(jid, agent_key);
                }
            }
        }

        if (!runtime->workspace.empty()) {
            spdlog::info("Using workspace '{}' for jid '{}'", runtime->workspace, jid);
        }

        auto inserted = runtimes.emplace(runtime_key, std::move(runtime));
        it = inserted.first;
    }
    return *it->second;
}

bool handle_channel_session_command(const InboundMessage &message, ConversationRuntime &runtime, SessionStore &session_store, ChannelManager &channel_manager, const Config &cfg) {
    if (message.content == "/help") {
        deliver_command_reply(message, channel_help_text(), channel_manager);
        return true;
    }

    if (message.content == "/new") {
        const auto previous_message_count = runtime.agent->history().size();
        const auto result = start_new_session(*runtime.agent, session_store, runtime.model, runtime.current_session_id, runtime.session_scope_key);
        dispatch_session_end(runtime.hook_manager, result.previous_session_id, previous_message_count);
        runtime.current_session_id.clear();
        session_store.clear_jid(message.jid, runtime.agent_key);
        runtime.persisted_message_count = 0;
        deliver_command_reply(message, describe_new_session_result(result, true), channel_manager);
        return true;
    }

    if (message.content == "/session") {
        deliver_command_reply(message, format_current_session(runtime.current_session_id, runtime.agent_key), channel_manager);
        return true;
    }

    if (message.content == "/sessions") {
        deliver_command_reply(message, format_scoped_sessions(session_store.list_sessions(runtime.session_scope_key), runtime.current_session_id), channel_manager);
        return true;
    }

    if (message.content == "/agent") {
        deliver_command_reply(message, "Current agent: " + runtime.agent_key, channel_manager);
        return true;
    }

    if (message.content == "/agents") {
        deliver_command_reply(message, format_agent_list(cfg, runtime.agent_key), channel_manager);
        return true;
    }

    if (message.content.starts_with("/resume ") || message.content.starts_with("/load ")) {
        const auto session_id = message.content.starts_with("/resume ") ? message.content.substr(8) : message.content.substr(6);
        if (session_id.empty()) {
            deliver_command_reply(message, "Usage: /resume <session-id>", channel_manager);
            return true;
        }

        const auto previous_session_id = runtime.current_session_id;
        const auto previous_message_count = runtime.agent->history().size();
        const auto resolved_session_id = resolve_requested_session(session_store, session_id, runtime.session_scope_key);
        if (!resolved_session_id.has_value()) {
            deliver_command_reply(message, "No saved sessions available in this scope.", channel_manager);
            return true;
        }

        if (!session_store.session_belongs_to_scope(*resolved_session_id, runtime.session_scope_key)) {
            deliver_command_reply(message, "That session does not belong to this conversation scope.", channel_manager);
            return true;
        }

        const auto load_result = load_session_into_agent(*resolved_session_id, *runtime.agent, session_store, runtime.current_session_id, runtime.session_scope_key);
        if (!load_result.loaded) {
            deliver_command_reply(message, load_result.status, channel_manager);
            return true;
        }

        if (previous_session_id != runtime.current_session_id) {
            dispatch_session_end(runtime.hook_manager, previous_session_id, previous_message_count);
            dispatch_session_start(runtime.hook_manager, runtime.current_session_id, runtime.agent->history().size());
        }

        runtime.persisted_message_count = runtime.agent->history().size();
        session_store.bind_jid(message.jid, *resolved_session_id, runtime.agent_key);
        deliver_command_reply(message, "Resumed session: " + runtime.current_session_id, channel_manager);
        return true;
    }

    if (message.content != "/compress") {
        return false;
    }

    const auto result = runtime.agent->compress_history();
    if (!result.compacted) {
        deliver_command_reply(message, result.status, channel_manager);
        return true;
    }

    persist_channel_session(message.jid, runtime, session_store);
    deliver_command_reply(message, "Compressed history: " + std::to_string(result.messages_before) + " -> " + std::to_string(result.messages_after) + " messages.",
                          channel_manager);
    return true;
}

} // namespace

ChannelApprovalCoordinator::ChannelApprovalCoordinator(std::chrono::milliseconds timeout)
: timeout_(timeout) {}

ToolApprovalCallback ChannelApprovalCoordinator::make_callback(const InboundMessage &message, ChannelManager &channel_manager) {
    if (!can_prompt_for_channel_approval(message)) {
        return {};
    }

    return [this, message, &channel_manager](const ToolUseBlock &, const std::string &prompt_text) {
        auto pending = std::make_shared<PendingApproval>();
        {
            std::scoped_lock lock(mutex_);
            pending->request_id = "shell-approval-" + std::to_string(++next_prompt_id_);
            pending->jid = message.jid;
            pending_by_request_id_[pending->request_id] = pending;
            pending_request_ids_by_jid_[message.jid].push_back(pending->request_id);
        }

        auto reply = prompt_text + "\nRequest: " + pending->request_id + "\nReply with `" + pending->request_id + " yes` to allow or `" + pending->request_id +
                     " no` to reject.";
        deliver_reply(message, reply, channel_manager);

        std::unique_lock lock(pending->mutex);
        const bool resolved = pending->cv.wait_for(lock, timeout_, [&pending] {
            return pending->resolved;
        });
        const auto approved = resolved && pending->approved;
        lock.unlock();

        clear_pending(pending);
        if (!resolved) {
            deliver_reply(message, "Shell approval timed out. The command was rejected.", channel_manager);
        }
        return approved;
    };
}

bool ChannelApprovalCoordinator::handle_inbound_message(const InboundMessage &message, ChannelManager &channel_manager) {
    std::vector<std::string> pending_request_ids;
    {
        std::scoped_lock lock(mutex_);
        pending_request_ids = pending_request_ids_for_jid(pending_request_ids_by_jid_, message.jid);
        if (pending_request_ids.empty()) {
            return false;
        }
    }

    const auto parsed = parse_channel_approval_reply(message.content);
    if (parsed.request_id.empty()) {
        deliver_reply(message, format_pending_channel_approval_prompt(pending_request_ids), channel_manager);
        return true;
    }

    std::shared_ptr<PendingApproval> pending;
    {
        std::scoped_lock lock(mutex_);
        const auto it = pending_by_request_id_.find(parsed.request_id);
        if (it != pending_by_request_id_.end() && it->second->jid == message.jid) {
            pending = it->second;
        }
    }

    if (pending == nullptr) {
        deliver_reply(message, format_pending_channel_approval_prompt(pending_request_ids), channel_manager);
        return true;
    }

    if (parsed.decision == ChannelApprovalDecision::invalid) {
        deliver_reply(message,
                      "Shell approval is pending. Reply with `" + pending->request_id + " yes` or `" + pending->request_id + " no`.",
                      channel_manager);
        return true;
    }

    {
        std::scoped_lock lock(pending->mutex);
        pending->resolved = true;
        pending->approved = parsed.decision == ChannelApprovalDecision::approve;
    }
    pending->cv.notify_all();
    return true;
}

void ChannelApprovalCoordinator::clear_pending(const std::shared_ptr<PendingApproval> &pending) {
    std::scoped_lock lock(mutex_);
    if (!pending->request_id.empty()) {
        pending_by_request_id_.erase(pending->request_id);
    }
    const auto it = pending_request_ids_by_jid_.find(pending->jid);
    if (it == pending_request_ids_by_jid_.end()) {
        return;
    }

    std::erase(it->second, pending->request_id);
    if (it->second.empty()) {
        pending_request_ids_by_jid_.erase(it);
    }
}

std::string resolve_agent_key_for_message(const InboundMessage &message, const std::unordered_map<std::string, std::string> &qq_bot_agents) {
    if (!message.agent_override.empty()) {
        return message.agent_override;
    }

    const auto bot_name = extract_qq_bot_name(message.jid);
    if (bot_name.empty()) {
        return "default";
    }

    if (const auto it = qq_bot_agents.find(bot_name); it != qq_bot_agents.end()) {
        return it->second;
    }
    return "default";
}

std::string resolve_reply_target(const InboundMessage &message) {
    if (message.reply_target == "cli") {
        return "cli";
    }
    if (message.reply_target.empty()) {
        if (message.jid.starts_with("heartbeat:")) {
            return "cli";
        }
        return message.jid;
    }
    return message.reply_target;
}

void deliver_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
    if (reply.empty()) {
        return;
    }

    const auto target = resolve_reply_target(message);
    if (target == "cli") {
        spdlog::info("Heartbeat reply [{}]: {}", message.jid, reply);
        return;
    }

    try {
        channel_manager.send(target, reply);
    } catch (const std::exception &e) {
        spdlog::error("Failed to deliver reply for jid '{}' to target '{}': {}", message.jid, target, e.what());
    }
}

void deliver_command_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
    deliver_reply(message, reply, channel_manager);
}

std::string build_skill_prompt_for_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg) {
    SkillLoader skill_loader;
    skill_loader.load_from_directories(resolve_skill_directories(cfg.skill_paths, runtime_cfg.workspace_root));
    return skill_loader.build_prompt_section();
}

namespace {

void process_channel_message(const InboundMessage &message, ChannelManager &channel_manager, std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes,
                             std::mutex &runtimes_mutex, const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs,
                             const std::unordered_map<std::string, std::string> &qq_bot_agents, MemoryStore *memory_store, SessionStore &session_store,
                             SubagentManager &subagent_manager, const Config &cfg, HookManager *hook_manager, CronStore *cron_store,
                             HeartbeatScheduler *heartbeat_scheduler, ChannelApprovalCoordinator &approval_coordinator) {
    try {
        auto &runtime = ensure_runtime_for_jid(message.jid, runtimes, runtimes_mutex, message, agent_configs, qq_bot_agents, memory_store, session_store, subagent_manager, cfg,
                                               hook_manager, cron_store, heartbeat_scheduler);
        if (handle_channel_session_command(message, runtime, session_store, channel_manager, cfg)) {
            return;
        }

        // Light context: clear history so agent runs with minimal context (system prompt + current message only)
        if (message.light_context) {
            runtime.agent->clear_history();
        }

        runtime.tool_context.approval_callback = approval_coordinator.make_callback(message, channel_manager);
        const auto reply = runtime.agent->run(message.content);

        // Skip session persistence for isolated heartbeat runs
        if (!message.isolated) {
            persist_channel_session(message.jid, runtime, session_store);
        }

        // Suppress HEARTBEAT_OK responses from heartbeat jobs
        if (should_suppress_heartbeat_reply(message.jid, reply, cfg.ack_max_chars)) {
            spdlog::debug("Suppressing HEARTBEAT_OK response from '{}'", message.jid);
            return;
        }

        deliver_reply(message, reply, channel_manager);
    } catch (const std::exception &e) {
        spdlog::error("Failed to process message for jid '{}': {}", message.jid, e.what());
    }
}

} // namespace

size_t default_serve_worker_count() {
    const auto hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 4;
    }
    return std::max<size_t>(2, hardware);
}

void run_channel_loop(MessageQueue &queue, ChannelManager &channel_manager, std::atomic<bool> &stop_requested, JidTaskRunner &task_runner,
                      const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const std::unordered_map<std::string, std::string> &qq_bot_agents,
                      MemoryStore *memory_store, SessionStore &session_store, SubagentManager &subagent_manager, const Config &cfg, HookManager *hook_manager,
                      CronStore *cron_store, HeartbeatScheduler *heartbeat_scheduler) {
    std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> runtimes;
    std::mutex runtimes_mutex;
    ChannelApprovalCoordinator approval_coordinator;

    while (!stop_requested.load()) {
        InboundMessage message;
        if (!queue.try_pop(message, serve_poll_interval)) {
            if (queue.is_shutdown()) {
                break;
            }
            continue;
        }

        if (message.jid.empty()) {
            continue;
        }

        if (approval_coordinator.handle_inbound_message(message, channel_manager)) {
            continue;
        }

        task_runner.submit(message.jid, [message, &channel_manager, &runtimes, &runtimes_mutex, &agent_configs, &qq_bot_agents, memory_store, &session_store, &subagent_manager,
                                         &cfg, hook_manager, cron_store, heartbeat_scheduler, &approval_coordinator] {
            process_channel_message(message, channel_manager, runtimes, runtimes_mutex, agent_configs, qq_bot_agents, memory_store, session_store, subagent_manager, cfg,
                                    hook_manager, cron_store, heartbeat_scheduler, approval_coordinator);
        });
    }

    task_runner.shutdown(true);

    std::scoped_lock lock(runtimes_mutex);
    for (auto &[runtime_key, runtime] : runtimes) {
        (void)runtime_key;
        dispatch_session_end(runtime->hook_manager, runtime->current_session_id, runtime->agent->history().size());
    }
}

void add_configured_channels(ChannelManager &channel_manager, const Config &cfg) {
    for (const auto &bot : cfg.qq_bots) {
        const bool has_qq_app_id = !bot.app_id.empty();
        const bool has_qq_secret = !bot.client_secret.empty();
        if (has_qq_app_id != has_qq_secret) {
            spdlog::warn("QQ channel configuration is incomplete for bot '{}'; both app_id and client_secret are required", bot.name.empty() ? std::string{"default"} : bot.name);
            continue;
        }
        if (!has_qq_app_id) {
            continue;
        }

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        channel_manager.add_channel(std::make_unique<QqChannel>(bot.name, bot.app_id, bot.client_secret));
#else
        spdlog::warn("QQ credentials are configured, but this build was compiled without QQ support. Rebuild with -DORANGUTAN_ENABLE_QQ_CHANNEL=ON");
#endif
    }
}

} // namespace orangutan::app
