#include "app/channel-serve.hpp"

#include "app/single-shot.hpp"
#include "app/slash-commands.hpp"
#include "app/runtime/agent-runtime.hpp"
#include "features/automation/runtime.hpp"
#include "features/agent/agent-loop.hpp"
#include "app/cli-ui.hpp"
#include "app/session-workflow.hpp"
#include "features/channel/qq/channel.hpp"
#include "features/heartbeat/protocol/heartbeat-ok.hpp"
#include "features/hooks/hook-manager.hpp"
#include "core/providers/provider.hpp"
#include "infra/execution/sender-utils.hpp"
#include "app/runtime/identity.hpp"
#include "features/skills/skill-loader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include "infra/format.hpp"
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace orangutan::app {

    namespace {

        constexpr auto serve_poll_interval = std::chrono::milliseconds(50);

        enum class channel_approval_decision {
            approve,
            deny,
            invalid,
        };

        struct ParsedChannelApprovalReply {
            std::string request_id;
            channel_approval_decision decision = channel_approval_decision::invalid;
        };

        struct ConversationRuntime {
            std::unique_ptr<AgentRuntimeBundle> runtime;
            HookManager *hook_manager = nullptr;
            std::shared_ptr<detail::ChannelCompletionResumeState> completion_resume_state;
            std::string runtime_key;
            std::string agent_key;
            std::string configured_model;
            std::vector<std::string> fallback_models;
            std::string workspace;
            std::string memory_scope;
            std::string session_scope_key;
            std::string current_session_id;
            size_t persisted_message_count = 0;

            [[nodiscard]]
            Provider *provider() const {
                return runtime != nullptr ? runtime->provider.get() : nullptr;
            }

            [[nodiscard]]
            ToolRegistry &tools() const {
                return runtime->tools;
            }

            [[nodiscard]]
            ToolRuntimeContext &tool_context() const {
                return runtime->tool_context;
            }

            [[nodiscard]]
            AgentLoop &agent() const {
                return *runtime->agent;
            }

            ~ConversationRuntime() {
                if (completion_resume_state == nullptr) {
                    return;
                }
                std::scoped_lock lock(completion_resume_state->mutex);
                completion_resume_state->agent = nullptr;
                completion_resume_state->provider = nullptr;
                completion_resume_state->hook_manager = nullptr;
                completion_resume_state->current_session_id = nullptr;
                completion_resume_state->persisted_message_count = nullptr;
                completion_resume_state->session_store = nullptr;
                completion_resume_state->channel_manager = nullptr;
                completion_resume_state->automation_runtime = nullptr;
            }
            ConversationRuntime() = default;
            ConversationRuntime(const ConversationRuntime &) = delete;
            ConversationRuntime &operator=(const ConversationRuntime &) = delete;
            ConversationRuntime(ConversationRuntime &&) = delete;
            ConversationRuntime &operator=(ConversationRuntime &&) = delete;
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

        channel_approval_decision parse_channel_approval_decision(std::string_view content) {
            const auto normalized = normalize_channel_approval_token(content);
            if (normalized.empty()) {
                return channel_approval_decision::invalid;
            }

            if (normalized == "y" || normalized == "yes" || normalized == "approve" || normalized == "approved" || normalized == "allow") {
                return channel_approval_decision::approve;
            }
            if (normalized == "n" || normalized == "no" || normalized == "deny" || normalized == "denied" || normalized == "reject") {
                return channel_approval_decision::deny;
            }
            return channel_approval_decision::invalid;
        }

        ParsedChannelApprovalReply parse_channel_approval_reply(const std::string &content) {
            ParsedChannelApprovalReply parsed;
            std::istringstream stream(content);
            for (std::string token; static_cast<bool>(stream >> token);) {
                const auto normalized = normalize_channel_approval_token(token);
                if (normalized.starts_with("shell-approval-")) {
                    parsed.request_id = normalized;
                    continue;
                }

                const auto decision = parse_channel_approval_decision(normalized);
                if (decision != channel_approval_decision::invalid) {
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

            std::string prompt = "Multiple shell approvals are pending. Reply with `<request-id> yes` or `<request-id> no`. Pending:";
            for (const auto &request_id : request_ids) {
                append(prompt, " {}", request_id);
            }
            return prompt;
        }

        std::vector<std::string> pending_request_ids_for_jid(const std::unordered_map<std::string, std::vector<std::string>> &pending_request_ids_by_jid, const std::string &jid) {
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

        std::unique_ptr<ConversationRuntime> make_conversation_runtime(const Config &app_cfg, const AgentRuntimeConfig &cfg, MemoryStore *memory_store,
                                                                       const RuntimeIdentity &identity, SubagentManager &subagent_manager, const std::string &raw_caller_id,
                                                                       HookManager *hook_manager, automation::Runtime *automation_runtime) {
            auto runtime = std::make_unique<ConversationRuntime>();
            auto completion_resume_state = std::make_shared<detail::ChannelCompletionResumeState>();
            runtime->runtime_key = identity.runtime_key;
            runtime->agent_key = cfg.agent_key;
            runtime->configured_model = cfg.model;
            runtime->fallback_models = cfg.fallback_models;
            runtime->workspace = identity.workspace;
            runtime->memory_scope = identity.memory_scope;
            runtime->session_scope_key = identity.runtime_key.empty() ? cfg.cli_runtime_key : identity.runtime_key;
            completion_resume_state->jid = raw_caller_id;
            completion_resume_state->agent_key = cfg.agent_key;
            completion_resume_state->configured_model = cfg.model;
            completion_resume_state->session_scope_key = runtime->session_scope_key;
            completion_resume_state->automation_runtime = automation_runtime;
            AgentRuntimeBuildInput input{
                .provider_name = cfg.provider_name,
                .api_key = cfg.api_key,
                .model = cfg.model,
                .fallback_models = cfg.fallback_models,
                .base_url = cfg.base_url,
                .agent_key = cfg.agent_key,
                .system_prompt = cfg.system_prompt,
                .workspace_root = cfg.workspace_root,
                .edit_mode = cfg.edit_mode,
                .thinking_budget = cfg.thinking_budget,
                .memory = cfg.memory,
                .permissions = cfg.permissions,
                .allowed_child_agents = cfg.allowed_child_agents,
                .identity = identity,
                .memory_store = memory_store,
                .current_session_id = &runtime->current_session_id,
                .subagent_manager = &subagent_manager,
                .runtime_origin = SubagentRuntimeOrigin::channel,
                .raw_caller_id = raw_caller_id,
                .automation_runtime = automation_runtime,
                .custom_tools = app_cfg.custom_tools,
                .mcp_servers = app_cfg.mcp_servers,
                .skill_paths = app_cfg.skill_paths,
                .hook_paths = app_cfg.hook_paths,
                .background_completion_runtime = automation_runtime != nullptr ? make_background_completion_runtime_bindings(
                                                                                     [automation_runtime](const automation::InboxItem &item) {
                                                                                         static_cast<void>(automation_runtime->store().insert_inbox(item));
                                                                                     },
                                                                                     detail::make_channel_completion_resume_callback(completion_resume_state))
                                                                               : nullptr,
            };
            runtime->runtime = std::make_unique<AgentRuntimeBundle>(build_agent_runtime(input));
            if (hook_manager != nullptr) {
                runtime->runtime->agent = std::make_unique<AgentLoop>(*runtime->runtime->provider, runtime->runtime->tools, runtime->runtime->system_prompt,
                                                                      runtime->runtime->memory.get(), runtime->runtime->skills_prompt, hook_manager);
                runtime->hook_manager = hook_manager;
            } else {
                runtime->hook_manager = runtime->runtime->hook_manager.get();
            }
            completion_resume_state->agent = runtime->runtime->agent.get();
            completion_resume_state->provider = runtime->runtime->provider.get();
            completion_resume_state->hook_manager = runtime->hook_manager;
            completion_resume_state->current_session_id = &runtime->current_session_id;
            completion_resume_state->persisted_message_count = &runtime->persisted_message_count;
            runtime->completion_resume_state = std::move(completion_resume_state);
            return runtime;
        }

        SessionMetadata make_channel_session_metadata(const ConversationRuntime &runtime, const std::string &jid, const std::string &model) {
            return SessionMetadata{
                .model = model,
                .scope_key = runtime.session_scope_key,
                .agent_key = runtime.agent_key,
                .origin_kind = "channel",
                .origin_ref = jid,
            };
        }

        SessionMetadata make_channel_session_metadata(const detail::ChannelCompletionResumeState &state, const std::string &model) {
            return SessionMetadata{
                .model = model,
                .scope_key = state.session_scope_key,
                .agent_key = state.agent_key,
                .origin_kind = "channel",
                .origin_ref = state.jid,
            };
        }

        void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store) {
            const auto &history = runtime.agent().history();
            if (history.empty()) {
                session_store.clear_jid(jid, runtime.agent_key);
                runtime.current_session_id.clear();
                runtime.persisted_message_count = 0;
                return;
            }

            const bool created_session = runtime.current_session_id.empty();
            const auto active_model =
                runtime.provider() != nullptr && !runtime.provider()->current_model().empty() ? runtime.provider()->current_model() : runtime.configured_model;
            const auto metadata = make_channel_session_metadata(runtime, jid, active_model);
            if (runtime.current_session_id.empty()) {
                runtime.current_session_id = session_store.save(history, metadata);
                runtime.persisted_message_count = history.size();
            } else if (history.size() > runtime.persisted_message_count) {
                session_store.append(runtime.current_session_id, history, runtime.persisted_message_count, metadata);
                runtime.persisted_message_count = history.size();
            } else {
                session_store.update(runtime.current_session_id, history, metadata);
                runtime.persisted_message_count = history.size();
            }

            session_store.bind_jid(jid, runtime.current_session_id, runtime.agent_key);
            if (created_session) {
                dispatch_session_start(runtime.hook_manager, runtime.current_session_id, history.size());
            }
        }

        std::optional<std::string> persist_channel_session(detail::ChannelCompletionResumeState &state) {
            if (state.agent == nullptr || state.current_session_id == nullptr || state.persisted_message_count == nullptr || state.session_store == nullptr) {
                return "channel runtime is no longer available";
            }

            const auto &history = state.agent->history();
            if (history.empty()) {
                state.session_store->clear_jid(state.jid, state.agent_key);
                state.current_session_id->clear();
                *state.persisted_message_count = 0;
                return std::nullopt;
            }

            const bool created_session = state.current_session_id->empty();
            const auto active_model = state.provider != nullptr && !state.provider->current_model().empty() ? state.provider->current_model() : state.configured_model;
            const auto metadata = make_channel_session_metadata(state, active_model);
            if (state.current_session_id->empty()) {
                *state.current_session_id = state.session_store->save(history, metadata);
                *state.persisted_message_count = history.size();
            } else if (history.size() > *state.persisted_message_count) {
                state.session_store->append(*state.current_session_id, history, *state.persisted_message_count, metadata);
                *state.persisted_message_count = history.size();
            } else {
                state.session_store->update(*state.current_session_id, history, metadata);
                *state.persisted_message_count = history.size();
            }

            state.session_store->bind_jid(state.jid, *state.current_session_id, state.agent_key);
            if (created_session) {
                dispatch_session_start(state.hook_manager, *state.current_session_id, history.size());
            }
            return std::nullopt;
        }

        ConversationRuntime &ensure_runtime_for_jid(const std::string &jid, std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes,
                                                    std::mutex &runtimes_mutex, const InboundMessage &message,
                                                    const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs,
                                                    const std::unordered_map<std::string, std::string> &qq_bot_agents, MemoryStore *memory_store, SessionStore &session_store,
                                                    SubagentManager &subagent_manager, const Config &cfg, HookManager *hook_manager, automation::Runtime *automation_runtime) {
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
                auto runtime = make_conversation_runtime(cfg, cfg_it->second, memory_store, identity, subagent_manager, jid, hook_manager, automation_runtime);
                if (!message.isolated) {
                    if (auto session_id = session_store.bound_session_for_jid(jid, agent_key); session_id.has_value()) {
                        try {
                            if (!session_store.session_belongs_to_scope(*session_id, runtime->session_scope_key)) {
                                spdlog::warn("Session {} does not belong to runtime scope '{}' for jid '{}' agent '{}'", *session_id, runtime->session_scope_key, jid, agent_key);
                                session_store.clear_jid(jid, agent_key);
                            } else {
                                runtime->agent().set_history(session_store.load(*session_id));
                                runtime->current_session_id = *session_id;
                                runtime->persisted_message_count = runtime->agent().history().size();
                                spdlog::info("Restored session {} for jid '{}' agent '{}'", *session_id, jid, agent_key);
                                dispatch_session_start(runtime->hook_manager, runtime->current_session_id, runtime->agent().history().size());
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

        bool handle_channel_session_command(const InboundMessage &message, ConversationRuntime &runtime, SessionStore &session_store, ChannelManager &channel_manager,
                                            const Config &cfg) {
            if (const auto reply = dispatch_shared_slash_command(
                    message.content,
                    {
                        .surface = slash_command_surface::channel,
                        .help =
                            [&] {
                                return SlashCommandReply{.handled = true, .text = channel_help_text()};
                            },
                        .new_session =
                            [&] {
                                const auto previous_message_count = runtime.agent().history().size();
                                const auto active_model =
                                    runtime.provider() != nullptr && !runtime.provider()->current_model().empty() ? runtime.provider()->current_model() : runtime.configured_model;
                                const auto result = start_new_session(runtime.agent(), session_store, runtime.current_session_id,
                                                                      make_channel_session_metadata(runtime, message.jid, active_model));
                                dispatch_session_end(runtime.hook_manager, result.previous_session_id, previous_message_count);
                                runtime.current_session_id.clear();
                                session_store.clear_jid(message.jid, runtime.agent_key);
                                runtime.persisted_message_count = 0;
                                return SlashCommandReply{.handled = true, .text = describe_new_session_result(result, true)};
                            },
                        .export_session =
                            [&] {
                                return SlashCommandReply{
                                    .handled = true,
                                    .text = describe_export_result(export_session_markdown(runtime.agent().history(), runtime.current_session_id, runtime.workspace)),
                                };
                            },
                        .compress =
                            [&] {
                                const auto result = runtime.agent().compress_history();
                                if (result.compacted) {
                                    persist_channel_session(message.jid, runtime, session_store);
                                }
                                return SlashCommandReply{.handled = true, .text = format_history_compaction_result(result)};
                            },
                        .session =
                            [&] {
                                return SlashCommandReply{.handled = true, .text = format_current_session(runtime.current_session_id, runtime.agent_key)};
                            },
                        .sessions =
                            [&] {
                                return SlashCommandReply{
                                    .handled = true,
                                    .text = format_scoped_sessions(session_store.list_sessions(runtime.session_scope_key), runtime.current_session_id),
                                };
                            },
                        .agent =
                            [&] {
                                return SlashCommandReply{.handled = true, .text = format_current_agent(runtime.agent_key)};
                            },
                        .status =
                            [&] {
                                return SlashCommandReply{
                                    .handled = true,
                                    .text = format_runtime_status(collect_runtime_status(runtime.agent(), *runtime.provider(), &runtime.tools(), runtime.current_session_id,
                                                                                         runtime.agent_key, runtime.configured_model, runtime.fallback_models,
                                                                                         runtime.session_scope_key)),
                                };
                            },
                        .agents =
                            [&] {
                                return SlashCommandReply{.handled = true, .text = format_agent_list(cfg, runtime.agent_key)};
                            },
                        .resume =
                            [&](const std::string &session_id) {
                                const auto previous_session_id = runtime.current_session_id;
                                const auto previous_message_count = runtime.agent().history().size();
                                const auto resolved_session_id = resolve_requested_session(session_store, session_id, runtime.session_scope_key, runtime.agent_key);
                                if (!resolved_session_id.has_value()) {
                                    return SlashCommandReply{.handled = true, .text = "No saved sessions available in this scope."};
                                }
                                if (!session_store.session_belongs_to_scope(*resolved_session_id, runtime.session_scope_key)) {
                                    return SlashCommandReply{.handled = true, .text = "That session does not belong to this conversation scope."};
                                }

                                const auto load_result = load_session_into_agent(*resolved_session_id, runtime.agent(), session_store, runtime.current_session_id,
                                                                                 runtime.session_scope_key, runtime.agent_key);
                                if (!load_result.loaded) {
                                    return SlashCommandReply{.handled = true, .text = load_result.status};
                                }

                                if (previous_session_id != runtime.current_session_id) {
                                    dispatch_session_end(runtime.hook_manager, previous_session_id, previous_message_count);
                                    dispatch_session_start(runtime.hook_manager, runtime.current_session_id, runtime.agent().history().size());
                                }

                                runtime.persisted_message_count = runtime.agent().history().size();
                                session_store.bind_jid(message.jid, *resolved_session_id, runtime.agent_key);
                                return SlashCommandReply{.handled = true, .text = "🧵 Resumed session: " + runtime.current_session_id};
                            },
                        .tool_registry = &runtime.tools(),
                    });
                reply.handled) {
                deliver_command_reply(message, reply.text, channel_manager);
                return true;
            }

            return false;
        }

    } // namespace

    ChannelApprovalCoordinator::ChannelApprovalCoordinator(std::chrono::milliseconds timeout)
    : timeout_(timeout) {}

    ToolApprovalCallback ChannelApprovalCoordinator::make_callback(const InboundMessage &message, ChannelManager &channel_manager, JidTaskRunner *task_runner) {
        if (!can_prompt_for_channel_approval(message)) {
            return {};
        }

        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return {};
            }
        }

        return [this, message, &channel_manager, task_runner](const ToolUseBlock &, const std::string &prompt_text) {
            struct WaitOutcome {
                std::shared_ptr<PendingApproval> pending;
                bool resolved = false;
                bool cancelled = false;
                bool approved = false;
            };

            auto pipeline = stdexec::just() | stdexec::then([this, &message]() {
                                auto pending = std::make_shared<PendingApproval>();
                                {
                                    std::scoped_lock lock(mutex_);
                                    if (shutting_down_) {
                                        return std::shared_ptr<PendingApproval>{};
                                    }
                                    pending->request_id = "shell-approval-" + std::to_string(++next_prompt_id_);
                                    pending->jid = message.jid;
                                    pending_by_request_id_[pending->request_id] = pending;
                                    pending_request_ids_by_jid_[message.jid].push_back(pending->request_id);
                                }
                                return pending;
                            }) |
                            stdexec::then([this, &message, &channel_manager, task_runner, &prompt_text](std::shared_ptr<PendingApproval> pending) {
                                if (pending == nullptr) {
                                    return WaitOutcome{};
                                }

                                auto reply = prompt_text + "\nRequest: " + pending->request_id + "\nReply with `" + pending->request_id + " yes` to allow or `" +
                                             pending->request_id + " no` to reject.";
                                deliver_reply(message, reply, channel_manager);

                                auto blocking_lease = task_runner != nullptr ? task_runner->acquire_blocking_lease() : JidTaskRunner::BlockingLease{};
                                std::unique_lock lock(pending->mutex);
                                const bool resolved = pending->cv.wait_for(lock, timeout_, [&pending] {
                                    return pending->resolved;
                                });
                                const bool cancelled = resolved && pending->cancelled;
                                const bool approved = resolved && !cancelled && pending->approved;
                                return WaitOutcome{
                                    .pending = std::move(pending),
                                    .resolved = resolved,
                                    .cancelled = cancelled,
                                    .approved = approved,
                                };
                            }) |
                            stdexec::then([this, &message, &channel_manager](const WaitOutcome &outcome) {
                                if (outcome.pending == nullptr) {
                                    return false;
                                }

                                clear_pending(outcome.pending);
                                if (!outcome.resolved && !outcome.cancelled) {
                                    deliver_reply(message, "Shell approval timed out. The command was rejected.", channel_manager);
                                }
                                return outcome.approved;
                            });

            auto [approved] = execution::sync_wait_or_throw(pipeline, "channel approval callback pipeline");
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

        if (parsed.decision == channel_approval_decision::invalid) {
            deliver_reply(message, "Shell approval is pending. Reply with `" + pending->request_id + " yes` or `" + pending->request_id + " no`.", channel_manager);
            return true;
        }

        {
            std::scoped_lock lock(pending->mutex);
            pending->resolved = true;
            pending->approved = parsed.decision == channel_approval_decision::approve;
        }
        pending->cv.notify_all();
        return true;
    }

    void ChannelApprovalCoordinator::shutdown() {
        std::vector<std::shared_ptr<PendingApproval>> pending;
        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return;
            }
            shutting_down_ = true;
            pending.reserve(pending_by_request_id_.size());
            for (const auto &[request_id, approval] : pending_by_request_id_) {
                static_cast<void>(request_id);
                pending.push_back(approval);
            }
            pending_by_request_id_.clear();
            pending_request_ids_by_jid_.clear();
        }

        for (const auto &approval : pending) {
            {
                std::scoped_lock lock(approval->mutex);
                approval->resolved = true;
                approval->approved = false;
                approval->cancelled = true;
            }
            approval->cv.notify_all();
        }
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

    namespace detail {

        ConversationRuntimeInspection inspect_conversation_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg, MemoryStore *memory_store,
                                                                   SubagentManager &subagent_manager, const std::string &raw_caller_id, HookManager *hook_manager,
                                                                   automation::Runtime *automation_runtime) {
            const auto identity = derive_channel_identity(runtime_cfg.workspace_root, raw_caller_id, runtime_cfg.agent_key);
            auto runtime = make_conversation_runtime(cfg, runtime_cfg, memory_store, identity, subagent_manager, raw_caller_id, hook_manager, automation_runtime);

            return ConversationRuntimeInspection{
                .tool_definitions = runtime->tools().definitions(),
                .runtime_origin = runtime->tool_context().runtime_origin,
                .raw_caller_id = runtime->tool_context().raw_caller_id,
                .has_agent = runtime->runtime != nullptr && runtime->runtime->agent != nullptr,
                .has_hook_manager = runtime->hook_manager != nullptr,
                .session_scope_key = runtime->session_scope_key,
                .configured_model = runtime->configured_model,
                .fallback_models = runtime->fallback_models,
            };
        }

        BackgroundCompletionResumeCallback make_channel_completion_resume_callback(const std::weak_ptr<ChannelCompletionResumeState> &weak_state) {
            return [weak_state](const std::string &message) -> std::optional<std::string> {
                const auto state = weak_state.lock();
                if (!state) {
                    return "channel runtime is no longer available";
                }

                std::scoped_lock lock(state->mutex);
                if (state->agent == nullptr) {
                    return "channel runtime is no longer available";
                }

                return run_completion_resume_message(
                    *state->agent, message, state->agent_key, state->automation_runtime,
                    [state](const std::string &reply) -> std::optional<std::string> {
                        if (const auto error = persist_channel_session(*state); error.has_value()) {
                            return error;
                        }
                        if (state->channel_manager != nullptr) {
                            deliver_reply(InboundMessage{.jid = state->jid}, reply, *state->channel_manager);
                        }
                        return std::nullopt;
                    },
                    true);
            };
        }

    } // namespace detail

    namespace {

        void process_channel_message(const InboundMessage &message, ChannelManager &channel_manager,
                                     std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes, std::mutex &runtimes_mutex,
                                     const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const std::unordered_map<std::string, std::string> &qq_bot_agents,
                                     MemoryStore *memory_store, SessionStore &session_store, SubagentManager &subagent_manager, const Config &cfg, HookManager *hook_manager,
                                     automation::Runtime *automation_runtime, ChannelApprovalCoordinator &approval_coordinator, JidTaskRunner &task_runner) {
            try {
                auto &runtime = ensure_runtime_for_jid(message.jid, runtimes, runtimes_mutex, message, agent_configs, qq_bot_agents, memory_store, session_store, subagent_manager,
                                                       cfg, hook_manager, automation_runtime);
                if (runtime.completion_resume_state != nullptr) {
                    std::scoped_lock lock(runtime.completion_resume_state->mutex);
                    runtime.completion_resume_state->session_store = &session_store;
                    runtime.completion_resume_state->channel_manager = &channel_manager;
                    runtime.completion_resume_state->jid = message.jid;
                }
                automation::with_agent_execution_lease(automation_runtime, runtime.agent_key, [&] {
                    if (handle_channel_session_command(message, runtime, session_store, channel_manager, cfg)) {
                        return;
                    }

                    // Light context: clear history so agent runs with minimal context (system prompt + current message only)
                    if (message.light_context) {
                        runtime.agent().clear_history();
                    }

                    runtime.tool_context().approval_callback = approval_coordinator.make_callback(message, channel_manager, &task_runner);
                    const auto reply = runtime.agent().run(message.content);

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
                });
            } catch (const std::exception &e) {
                spdlog::error("Failed to process message for jid '{}': {}", message.jid, e.what());
                deliver_reply(message, "Error: " + std::string(e.what()), channel_manager);
            } catch (...) {
                spdlog::error("Failed to process message for jid '{}': unknown exception", message.jid);
                deliver_reply(message, "Error: internal failure while processing the message.", channel_manager);
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
                          automation::Runtime *automation_runtime) {
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
                                             &cfg, hook_manager, automation_runtime, &approval_coordinator, &task_runner] {
                process_channel_message(message, channel_manager, runtimes, runtimes_mutex, agent_configs, qq_bot_agents, memory_store, session_store, subagent_manager, cfg,
                                        hook_manager, automation_runtime, approval_coordinator, task_runner);
            });
        }

        approval_coordinator.shutdown();
        task_runner.shutdown(true);

        std::scoped_lock lock(runtimes_mutex);
        for (auto &[runtime_key, runtime] : runtimes) {
            static_cast<void>(runtime_key);
            dispatch_session_end(runtime->hook_manager, runtime->current_session_id, runtime->agent().history().size());
        }
    }

    void add_configured_channels(ChannelManager &channel_manager, const Config &cfg) {
        for (const auto &bot : cfg.qq_bots) {
            const bool has_qq_app_id = !bot.app_id.empty();
            const bool has_qq_secret = !bot.client_secret.empty();
            if (has_qq_app_id != has_qq_secret) {
                spdlog::warn("QQ channel configuration is incomplete for bot '{}'; both app_id and client_secret are required",
                             bot.name.empty() ? std::string{"default"} : bot.name);
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
