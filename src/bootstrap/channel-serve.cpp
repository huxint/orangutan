#include "bootstrap/channel-serve.hpp"

#include "bootstrap/channel-serve-approval.hpp"
#include "cli/single-shot.hpp"
#include "cli/slash-commands.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/runtime-assembler.hpp"
#include "permissions/approval-signature.hpp"
#include "permissions/permission-display.hpp"
#include "permissions/permission-state.hpp"
#include "automation/runtime.hpp"
#include "agent/agent-loop.hpp"
#include "cli/cli-ui.hpp"
#include "cli/session-workflow.hpp"
#include "channel/qq/qq-channel.hpp"
#include "channel/qq/qq-approval-keyboard.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "heartbeat/heartbeat-ok.hpp"
#include "hooks/hook-manager.hpp"
#include "providers/provider.hpp"
#include "utils/format.hpp"
#include "utils/scope-exit.hpp"
#include "utils/sender-utils.hpp"
#include "utils/utf8-policy.hpp"
#include "bootstrap/identity.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap {

    using detail::can_prompt_for_channel_approval;
    using detail::channel_approval_decision;
    using detail::ChannelCompletionResumeState;
    using detail::ConversationRuntime;
    using detail::extract_qq_bot_name;
    using detail::format_channel_approval_request_id;
    using detail::format_qq_approval_delivery_failure;
    using detail::format_qq_channel_approval_card_markdown;
    using detail::format_qq_tool_progress_markdown;
    using detail::is_qq_custom_keyboard_blocked_error;
    using detail::make_channel_session_metadata;
    using detail::make_conversation_runtime;
    using detail::parse_channel_approval_reply;
    using detail::pending_request_ids_for_jid;
    using detail::persist_channel_session;
    using detail::qq_keyboard_capability_key;
    using detail::rehydrate_session_permissions;
    using detail::restore_bound_channel_session;
    using detail::should_send_qq_tool_progress_at_start;

    namespace {

        namespace cli = orangutan::cli;

        constexpr auto SERVE_POLL_INTERVAL = std::chrono::milliseconds(50);
        constexpr auto CHANNEL_RUNTIME_PRUNE_INTERVAL = std::chrono::seconds(15);
        constexpr auto CHANNEL_RUNTIME_IDLE_TTL = std::chrono::minutes(15);
        constexpr std::size_t MAX_CHANNEL_RUNTIME_CACHE_SIZE = 64;
        constexpr auto PARKED_CHANNEL_RESUME_TTL = std::chrono::hours(24);
        constexpr std::size_t MAX_PARKED_CHANNEL_RESUME_CACHE_SIZE = 256;

        using ParkedResumeStateMap = std::unordered_map<std::string, std::shared_ptr<ChannelCompletionResumeState>>;

        void release_runtime_use(ConversationRuntime &runtime, std::mutex &runtimes_mutex) {
            std::scoped_lock lock(runtimes_mutex);
            runtime.last_used_at = std::chrono::steady_clock::now();
            if (runtime.active_operations > 0) {
                --runtime.active_operations;
            }
        }

        void erase_parked_runtime(ParkedResumeStateMap &parked_resume_states, const std::string &runtime_key, orchestration::OrchestrationManager *orchestration_manager) {
            if (orchestration_manager != nullptr) {
                orchestration_manager->unregister_runtime_notification_handler(runtime_key);
            }
            parked_resume_states.erase(runtime_key);
        }

        [[nodiscard]]
        std::chrono::steady_clock::time_point parked_state_timestamp(const std::shared_ptr<ChannelCompletionResumeState> &state) {
            if (state == nullptr) {
                return std::chrono::steady_clock::time_point::min();
            }
            return state->parked_at == std::chrono::steady_clock::time_point{} ? std::chrono::steady_clock::time_point::min() : state->parked_at;
        }

        void prune_parked_resume_states(ParkedResumeStateMap &parked_resume_states, orchestration::OrchestrationManager *orchestration_manager) {
            const auto now = std::chrono::steady_clock::now();

            for (auto it = parked_resume_states.begin(); it != parked_resume_states.end();) {
                const auto parked_at = parked_state_timestamp(it->second);
                const bool expired = parked_at == std::chrono::steady_clock::time_point::min() || (now - parked_at) >= PARKED_CHANNEL_RESUME_TTL;
                if (expired) {
                    const auto runtime_key = it->first;
                    ++it;
                    erase_parked_runtime(parked_resume_states, runtime_key, orchestration_manager);
                    continue;
                }
                ++it;
            }

            if (parked_resume_states.size() <= MAX_PARKED_CHANNEL_RESUME_CACHE_SIZE) {
                return;
            }

            std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> eviction_order;
            eviction_order.reserve(parked_resume_states.size());
            for (const auto &[runtime_key, state] : parked_resume_states) {
                eviction_order.emplace_back(runtime_key, parked_state_timestamp(state));
            }

            std::ranges::sort(eviction_order, [](const auto &left, const auto &right) {
                return left.second < right.second;
            });

            for (const auto &[runtime_key, parked_at] : eviction_order) {
                static_cast<void>(parked_at);
                if (parked_resume_states.size() <= MAX_PARKED_CHANNEL_RESUME_CACHE_SIZE) {
                    break;
                }
                erase_parked_runtime(parked_resume_states, runtime_key, orchestration_manager);
            }
        }

        void park_runtime(ParkedResumeStateMap &parked_resume_states, const std::string &runtime_key, ConversationRuntime &runtime) {
            if (auto parked_state = detail::park_conversation_runtime(runtime); parked_state != nullptr) {
                parked_resume_states.insert_or_assign(runtime_key, std::move(parked_state));
            }
        }

        void prune_inactive_runtimes(std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes, ParkedResumeStateMap &parked_resume_states,
                                     orchestration::OrchestrationManager *orchestration_manager, std::mutex &runtimes_mutex) {
            std::scoped_lock lock(runtimes_mutex);
            const auto now = std::chrono::steady_clock::now();

            for (auto it = runtimes.begin(); it != runtimes.end();) {
                const auto &runtime = it->second;
                const bool idle_for_too_long = runtime != nullptr && runtime->active_operations == 0 && (now - runtime->last_used_at) >= CHANNEL_RUNTIME_IDLE_TTL;
                if (idle_for_too_long) {
                    if (runtime != nullptr) {
                        park_runtime(parked_resume_states, it->first, *runtime);
                    }
                    it = runtimes.erase(it);
                    continue;
                }
                ++it;
            }

            if (runtimes.size() > MAX_CHANNEL_RUNTIME_CACHE_SIZE) {
                std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> eviction_order;
                eviction_order.reserve(runtimes.size());
                for (const auto &[runtime_key, runtime] : runtimes) {
                    if (runtime != nullptr && runtime->active_operations == 0) {
                        eviction_order.emplace_back(runtime_key, runtime->last_used_at);
                    }
                }

                std::ranges::sort(eviction_order, [](const auto &left, const auto &right) {
                    return left.second < right.second;
                });

                for (const auto &entry : eviction_order) {
                    if (runtimes.size() <= MAX_CHANNEL_RUNTIME_CACHE_SIZE) {
                        break;
                    }
                    if (auto it = runtimes.find(entry.first); it != runtimes.end() && it->second != nullptr) {
                        park_runtime(parked_resume_states, it->first, *it->second);
                    }
                    runtimes.erase(entry.first);
                }
            }

            prune_parked_resume_states(parked_resume_states, orchestration_manager);
        }

        std::string describe_attachment_for_agent(const Attachment &attachment, std::size_t index) {
            std::string line = fmt::format("- [{}] {}", index, attachment.filename.empty() ? "unnamed-attachment" : attachment.filename);
            if (!attachment.content_type.empty()) {
                line.append(" (");
                line.append(attachment.content_type);
                line.push_back(')');
            }
            if (attachment.width > 0 && attachment.height > 0) {
                utils::format_to(line, " {}x{}", attachment.width, attachment.height);
            }
            if (attachment.size > 0) {
                utils::format_to(line, " {} bytes", attachment.size);
            }
            if (!attachment.local_path.empty()) {
                line.append(" -> ");
                line.append(attachment.local_path);
            } else if (!attachment.url.empty()) {
                line.append(" downloadable");
            }
            return line;
        }

        std::string describe_inbound_event(const InboundMessage &message) {
            if (message.event_kind == inbound_event_kind::message) {
                return {};
            }

            if (!message.reaction.has_value()) {
                return {};
            }

            const auto &reaction = *message.reaction;
            const auto actor = message.sender_name.empty() ? message.sender : message.sender_name;
            const auto *const event_label = message.event_kind == inbound_event_kind::reaction_added ? "added" : "removed";
            std::string prompt = "[Reaction event]\n";
            utils::format_to(prompt, "{} {} a reaction", actor.empty() ? std::string{"A user"} : actor, event_label);
            if (!reaction.emoji_id.empty()) {
                utils::format_to(prompt, " (`{}`)", reaction.emoji_id);
            }
            if (!reaction.target_id.empty()) {
                utils::format_to(prompt, " on message `{}`", reaction.target_id);
            }
            prompt.push_back('.');
            return prompt;
        }

        bool is_supported_inbound_event(const InboundMessage &message) {
            if (message.is_user_message()) {
                return true;
            }
            return message.reaction.has_value();
        }

        std::string build_agent_input(const InboundMessage &message) {
            std::string prompt = describe_inbound_event(message);

            if (!message.referenced_content.empty()) {
                prompt.append("[Quoted/referenced message]\n");
                prompt.append(message.referenced_content);
                prompt.append("\n\n[User's reply]\n");
            }

            prompt.append(message.content);

            if (!message.attachments.empty()) {
                if (!prompt.empty()) {
                    prompt.append("\n\n");
                }
                prompt.append("[Current message attachments]\n");
                for (std::size_t index = 0; index < message.attachments.size(); ++index) {
                    prompt.append(describe_attachment_for_agent(message.attachments[index], index));
                    prompt.push_back('\n');
                }
                prompt.append("Attachments have been auto-downloaded to the workspace. "
                              "For images, use the file_read tool on the local path to view them immediately. "
                              "For audio/voice files, note the path for reference.");
            }
            auto policy = utf8_policy::display_policy();
            policy.invalid_utf8 = utf8_policy::invalid_utf8_mode::replace_invalid;
            auto sanitized = utf8_policy::canonicalize(prompt, policy);
            return sanitized.has_value() ? std::move(sanitized->value) : prompt;
        }

        ConversationRuntime &ensure_runtime_for_jid(const std::string &jid, std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes,
                                                    ParkedResumeStateMap &parked_resume_states, std::mutex &runtimes_mutex, const InboundMessage &message,
                                                    const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs,
                                                    const utils::transparent_string_unordered_map<std::string> &qq_bot_agents, MemoryStore *memory_store,
                                                    SessionStore &session_store, orchestration::OrchestrationManager *orchestration_manager, const Config &cfg,
                                                    HookManager *hook_manager, automation::AutomationRuntime *automation_runtime, orchestration::TeamManager *team_manager,
                                                    orchestration::AgentMailbox *mailbox) {
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
                std::shared_ptr<ChannelCompletionResumeState> parked_state;
                if (auto parked_it = parked_resume_states.find(runtime_key); parked_it != parked_resume_states.end()) {
                    parked_state = std::move(parked_it->second);
                    parked_resume_states.erase(parked_it);
                }
                auto runtime = make_conversation_runtime(cfg, cfg_it->second, memory_store, identity, orchestration_manager, jid, hook_manager, automation_runtime, team_manager,
                                                         mailbox, parked_state);
                if (!message.isolated) {
                    restore_bound_channel_session(session_store, jid, *runtime);
                }

                if (!runtime->workspace.empty()) {
                    spdlog::info("using workspace '{}' for jid '{}'", runtime->workspace, jid);
                }

                it = runtimes.try_emplace(runtime_key, std::move(runtime)).first;
            }
            it->second->last_used_at = std::chrono::steady_clock::now();
            ++it->second->active_operations;
            return *it->second;
        }

        bool handle_channel_session_command(const InboundMessage &message, ConversationRuntime &runtime, SessionStore &session_store, ChannelManager &channel_manager,
                                            const Config &cfg) {
            if (const auto reply = cli::dispatch_shared_slash_command(
                    message.content,
                    {
                        .surface = cli::slash_command_surface::channel,
                        .help =
                            [&] {
                                return cli::SlashCommandReply{.handled = true, .text = cli::channel_help_text()};
                            },
                        .new_session =
                            [&] {
                                const auto previous_message_count = runtime.agent().history().size();
                                const auto active_model =
                                    runtime.provider() != nullptr && !runtime.provider()->current_model().empty() ? runtime.provider()->current_model() : runtime.configured_model;
                                const auto distillation_dispatcher =
                                    runtime.provider() != nullptr ? cli::make_background_session_distillation_dispatcher(
                                                                        runtime.completion_resume_state != nullptr ? runtime.completion_resume_state->automation_runtime : nullptr,
                                                                        *runtime.provider(), runtime.provider_route, runtime.runtime->memory.get())
                                                                  : cli::SessionDistillationDispatcher{};
                                const auto result = cli::start_new_session(runtime.agent(), session_store, runtime.current_session_id,
                                                                           make_channel_session_metadata(runtime, message.jid, active_model), distillation_dispatcher);
                                dispatch_session_end(runtime.hook_manager, result.previous_session_id, previous_message_count);
                                runtime.current_session_id.clear();
                                session_store.clear_jid(message.jid, runtime.agent_key);
                                rehydrate_session_permissions(session_store, runtime.current_session_id, runtime.runtime.get());
                                runtime.persisted_message_count = 0;
                                return cli::SlashCommandReply{.handled = true, .text = cli::describe_new_session_result(result, true)};
                            },
                        .export_session =
                            [&] {
                                return cli::SlashCommandReply{
                                    .handled = true,
                                    .text = cli::describe_export_result(cli::export_session_markdown(runtime.agent().history(), runtime.current_session_id, runtime.workspace)),
                                };
                            },
                        .compress =
                            [&] {
                                const auto result = runtime.agent().compress_history();
                                if (result.compacted) {
                                    persist_channel_session(message.jid, runtime, session_store);
                                }
                                return cli::SlashCommandReply{.handled = true, .text = cli::format_history_compaction_result(result)};
                            },
                        .session =
                            [&] {
                                return cli::SlashCommandReply{.handled = true, .text = cli::format_current_session(runtime.current_session_id, runtime.agent_key)};
                            },
                        .sessions =
                            [&] {
                                return cli::SlashCommandReply{
                                    .handled = true,
                                    .text = cli::format_scoped_sessions(session_store.list_sessions(runtime.session_scope_key), runtime.current_session_id),
                                };
                            },
                        .agent =
                            [&] {
                                return cli::SlashCommandReply{.handled = true, .text = cli::format_current_agent(runtime.agent_key)};
                            },
                        .status =
                            [&] {
                                return cli::SlashCommandReply{
                                    .handled = true,
                                    .text = cli::format_runtime_status(cli::collect_runtime_status(runtime.agent(), *runtime.provider(), &runtime.tools(),
                                                                                                   runtime.current_session_id, runtime.agent_key, runtime.configured_model,
                                                                                                   runtime.fallback_models, runtime.session_scope_key)),
                                };
                            },
                        .agents =
                            [&] {
                                return cli::SlashCommandReply{.handled = true, .text = cli::format_agent_list(cfg, runtime.agent_key)};
                            },
                        .resume =
                            [&](const std::string &session_id) {
                                const auto previous_session_id = runtime.current_session_id;
                                const auto previous_message_count = runtime.agent().history().size();
                                const auto resolved_session_id = cli::resolve_requested_session(session_store, session_id, runtime.session_scope_key, runtime.agent_key);
                                if (!resolved_session_id.has_value()) {
                                    return cli::SlashCommandReply{.handled = true, .text = "No saved sessions available in this scope."};
                                }
                                if (!session_store.session_belongs_to_scope(*resolved_session_id, runtime.session_scope_key)) {
                                    return cli::SlashCommandReply{.handled = true, .text = "That session does not belong to this conversation scope."};
                                }

                                const auto load_result = cli::load_session_into_agent(*resolved_session_id, runtime.agent(), session_store, runtime.current_session_id,
                                                                                      runtime.session_scope_key, runtime.agent_key);
                                if (!load_result.loaded) {
                                    return cli::SlashCommandReply{.handled = true, .text = load_result.status};
                                }

                                if (previous_session_id != runtime.current_session_id) {
                                    dispatch_session_end(runtime.hook_manager, previous_session_id, previous_message_count);
                                    dispatch_session_start(runtime.hook_manager, runtime.current_session_id, runtime.agent().history().size());
                                }

                                rehydrate_session_permissions(session_store, runtime.current_session_id, runtime.runtime.get());
                                runtime.persisted_message_count = runtime.agent().history().size();
                                session_store.bind_jid(message.jid, *resolved_session_id, runtime.agent_key);
                                return cli::SlashCommandReply{.handled = true, .text = "🧵 Resumed session: " + runtime.current_session_id};
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

    ChannelApprovalGate::ChannelApprovalGate(std::chrono::milliseconds timeout)
    : timeout_(timeout) {}

    ApprovalCallback ChannelApprovalGate::make_callback(const InboundMessage &message, ChannelManager &channel_manager, JidTaskRunner *task_runner,
                                                        tools::PermissionRuleMutationCallback permission_rule_mutator) {
        if (!can_prompt_for_channel_approval(message)) {
            return {};
        }

        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return {};
            }
        }

        return
            [this, message, &channel_manager, task_runner, permission_rule_mutator = std::move(permission_rule_mutator)](const ToolUse &call, const PermissionDecision &decision) {
                struct WaitOutcome {
                    std::shared_ptr<PendingApproval> pending;
                    bool resolved = false;
                    bool cancelled = false;
                    bool approved = false;
                    bool always_allow = false;
                };

                auto pipeline = stdexec::just() | stdexec::then([this, &message]() {
                                    auto pending = std::make_shared<PendingApproval>();
                                    {
                                        std::scoped_lock lock(mutex_);
                                        if (shutting_down_) {
                                            return std::shared_ptr<PendingApproval>{};
                                        }
                                        pending->request_id = format_channel_approval_request_id(++next_prompt_id_);
                                        pending->jid = message.jid;
                                        pending_by_request_id_[pending->request_id] = pending;
                                        pending_request_ids_by_jid_[message.jid].push_back(pending->request_id);
                                    }
                                    return pending;
                                }) |
                                stdexec::then([this, &message, &channel_manager, task_runner, &call, &decision](std::shared_ptr<PendingApproval> pending) {
                                    if (pending == nullptr) {
                                        return WaitOutcome{};
                                    }

                                    const auto include_allow_always = permissions::derive_approval_signature(call).always_allow_eligible;
                                    const auto target = resolve_reply_target(message);
                                    const auto qq_keyboard_key = qq_keyboard_capability_key(target);
                                    {
                                        std::scoped_lock lock(pending->mutex);
                                        pending->allow_always_eligible = include_allow_always;
                                    }
                                    const auto qq_keyboard_disabled = [&] {
                                        std::scoped_lock lock(mutex_);
                                        return qq_keyboard_disabled_keys_.contains(qq_keyboard_key);
                                    }();

                                    if (qq_keyboard_disabled) {
                                        clear_pending(pending);
                                        deliver_reply(message, format_qq_approval_delivery_failure(call, decision, true), channel_manager);
                                        return WaitOutcome{
                                            .pending = std::move(pending),
                                            .resolved = true,
                                            .cancelled = true,
                                        };
                                    }

                                    try {
                                        // Match openclaw: QQ custom keyboards must be sent as a fresh message, not a passive reply carrying msg_id.
                                        channel_manager.send_keyboard(target, format_qq_channel_approval_card_markdown(call, decision),
                                                                      channel::qq::build_approval_keyboard(pending->request_id, include_allow_always), "", "");
                                    } catch (const std::exception &e) {
                                        if (is_qq_custom_keyboard_blocked_error(e.what())) {
                                            {
                                                std::scoped_lock lock(mutex_);
                                                qq_keyboard_disabled_keys_.insert(qq_keyboard_key);
                                            }
                                            spdlog::warn("qq custom keyboard is unavailable for bot '{}'; rejecting tool call: {}", qq_keyboard_key, e.what());
                                            clear_pending(pending);
                                            deliver_reply(message, format_qq_approval_delivery_failure(call, decision, true), channel_manager);
                                            return WaitOutcome{
                                                .pending = std::move(pending),
                                                .resolved = true,
                                                .cancelled = true,
                                            };
                                        } else {
                                            spdlog::warn("failed to deliver approval prompt for jid '{}': {}", message.jid, e.what());
                                            clear_pending(pending);
                                            deliver_reply(message, format_qq_approval_delivery_failure(call, decision, false), channel_manager);
                                            return WaitOutcome{
                                                .pending = std::move(pending),
                                                .resolved = true,
                                                .cancelled = true,
                                            };
                                        }
                                    }

                                    auto blocking_lease = task_runner != nullptr ? task_runner->acquire_blocking_lease() : JidTaskRunner::BlockingLease{};
                                    std::unique_lock lock(pending->mutex);
                                    const bool resolved = pending->cv.wait_for(lock, timeout_, [&pending] {
                                        return pending->resolved;
                                    });
                                    const bool cancelled = resolved && pending->cancelled;
                                    const bool approved = resolved && !cancelled && pending->approved;
                                    const bool always_allow = resolved && !cancelled && pending->always_allow;
                                    return WaitOutcome{
                                        .pending = std::move(pending),
                                        .resolved = resolved,
                                        .cancelled = cancelled,
                                        .approved = approved,
                                        .always_allow = always_allow,
                                    };
                                }) |
                                stdexec::then([this, &message, &channel_manager, &call, permission_rule_mutator](const WaitOutcome &outcome) {
                                    if (outcome.pending == nullptr) {
                                        return false;
                                    }

                                    clear_pending(outcome.pending);
                                    if (outcome.approved && outcome.always_allow) {
                                        if (const auto rule = permissions::make_session_allow_rule(call); rule.has_value()) {
                                            if (permission_rule_mutator) {
                                                permission_rule_mutator(*rule);
                                            }
                                        }
                                    }
                                    if (!outcome.resolved && !outcome.cancelled) {
                                        deliver_reply(message, "Approval timed out. The tool call was rejected.", channel_manager);
                                    }
                                    return outcome.approved;
                                });

                auto [approved] = execution::sync_wait_or_throw(pipeline, "channel approval callback pipeline");
                return approved;
            };
    }

    bool ChannelApprovalGate::handle_inbound_message(const InboundMessage &message, ChannelManager &channel_manager) {
        if (!message.is_user_message()) {
            return false;
        }

        std::vector<std::string> pending_request_ids;
        {
            std::scoped_lock lock(mutex_);
            pending_request_ids = pending_request_ids_for_jid(pending_request_ids_by_jid_, message.jid);
            if (pending_request_ids.empty()) {
                return false;
            }
        }

        std::shared_ptr<PendingApproval> pending;
        {
            std::scoped_lock lock(mutex_);
            for (const auto &request_id : pending_request_ids) {
                const auto it = pending_by_request_id_.find(request_id);
                if (it == pending_by_request_id_.end() || it->second->jid != message.jid) {
                    continue;
                }
                pending = it->second;
                break;
            }
        }

        const auto parsed = parse_channel_approval_reply(message.content);
        if (!parsed.has_value()) {
            return true;
        }

        if (pending == nullptr || pending->request_id != parsed->request_id) {
            pending.reset();
            std::scoped_lock lock(mutex_);
            const auto it = pending_by_request_id_.find(parsed->request_id);
            if (it != pending_by_request_id_.end() && it->second->jid == message.jid) {
                pending = it->second;
            }
        }

        if (pending == nullptr) {
            return true;
        }

        const auto allow_always_eligible = [&pending] {
            std::scoped_lock lock(pending->mutex);
            return pending->allow_always_eligible;
        }();

        if (parsed->decision == channel_approval_decision::approve_always && !allow_always_eligible) {
            return true;
        }

        {
            std::scoped_lock lock(pending->mutex);
            pending->resolved = true;
            pending->approved = parsed->decision == channel_approval_decision::approve_once || parsed->decision == channel_approval_decision::approve_always;
            pending->always_allow = parsed->decision == channel_approval_decision::approve_always;
        }
        pending->cv.notify_all();
        return true;
    }

    void ChannelApprovalGate::shutdown() {
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

    void ChannelApprovalGate::clear_pending(const std::shared_ptr<PendingApproval> &pending) {
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

    std::string resolve_agent_key_for_message(const InboundMessage &message, const utils::transparent_string_unordered_map<std::string> &qq_bot_agents) {
        if (!message.agent_override.empty()) {
            return message.agent_override;
        }

        const auto bot_name = extract_qq_bot_name(message.jid);
        if (bot_name.empty()) {
            return "default";
        }

        if (const auto it = utils::transparent_find(qq_bot_agents, bot_name); it != qq_bot_agents.end()) {
            return it->second;
        }
        return "default";
    }

    namespace {

        void process_channel_message(const InboundMessage &message, ChannelManager &channel_manager,
                                     std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> &runtimes, ParkedResumeStateMap &parked_resume_states,
                                     std::mutex &runtimes_mutex, const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs,
                                     const utils::transparent_string_unordered_map<std::string> &qq_bot_agents, MemoryStore *memory_store, SessionStore &session_store,
                                     orchestration::OrchestrationManager *orchestration_manager, const Config &cfg, HookManager *hook_manager,
                                     automation::AutomationRuntime *automation_runtime, ChannelApprovalGate &approval_gate, JidTaskRunner &task_runner,
                                     orchestration::TeamManager *team_manager, orchestration::AgentMailbox *mailbox) {
            try {
                auto &runtime = ensure_runtime_for_jid(message.jid, runtimes, parked_resume_states, runtimes_mutex, message, agent_configs, qq_bot_agents, memory_store,
                                                       session_store, orchestration_manager, cfg, hook_manager, automation_runtime, team_manager, mailbox);
                const auto release_runtime = utils::scope_exit([&runtime, &runtimes_mutex] {
                    release_runtime_use(runtime, runtimes_mutex);
                });
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

                    auto &tool_context = runtime.tool_context();
                    tool_context.current_message_attachments = message.attachments;
                    tool_context.attachment_download_callback = [&channel_manager, jid = message.jid](const Attachment &attachment, const std::string &destination_path) {
                        return channel_manager.download_attachment(jid, attachment, destination_path);
                    };
                    const auto restore_tool_context = utils::scope_exit([&tool_context] {
                        tool_context.current_message_attachments.clear();
                        tool_context.attachment_download_callback = {};
                        tool_context.approval_callback = {};
                    });

                    // Auto-download attachments so the agent can process them immediately
                    auto effective_message = message;
                    if (!effective_message.attachments.empty() && tool_context.attachment_download_callback) {
                        const auto ws = std::filesystem::path(runtime.workspace.empty() ? "." : runtime.workspace);
                        for (std::size_t att_idx = 0; att_idx < effective_message.attachments.size(); ++att_idx) {
                            auto &att = effective_message.attachments[att_idx];
                            if (!att.download_pending || att.url.empty()) {
                                continue;
                            }
                            try {
                                const auto name = att.filename.empty() ? "attachment-" + std::to_string(att_idx) : att.filename;
                                att = tool_context.attachment_download_callback(att, (ws / ".orangutan" / "attachments" / name).string());
                                tool_context.current_message_attachments[att_idx] = att;
                            } catch (const std::exception &e) {
                                spdlog::warn("auto-download attachment [{}] failed: {}", att_idx, e.what());
                            }
                        }
                    }

                    // Light context: clear history so agent runs with minimal context (system prompt + current message only)
                    if (message.light_context) {
                        runtime.agent().clear_history();
                    }

                    std::unordered_set<std::string> qq_approval_prompted_tool_ids;
                    auto approval_callback = approval_gate.make_callback(
                        message, channel_manager, &task_runner,
                        [&session_store, current_session_id = &runtime.current_session_id, base_mutator = tool_context.permission_rule_mutator](PermissionRule rule) {
                            if (base_mutator) {
                                base_mutator(rule);
                            }
                            if (current_session_id != nullptr && !current_session_id->empty()) {
                                session_store.save_session_permission_rule(*current_session_id, std::move(rule));
                            }
                        });
                    if (approval_callback != nullptr) {
                        tool_context.approval_callback = [approval_callback = std::move(approval_callback),
                                                          &qq_approval_prompted_tool_ids](const ToolUse &call, const PermissionDecision &decision) mutable {
                            if (!call.id.empty()) {
                                qq_approval_prompted_tool_ids.insert(call.id);
                            }
                            return approval_callback(call, decision);
                        };
                    } else {
                        tool_context.approval_callback = {};
                    }
                    channel_manager.start_typing(message.jid, message.message_id);
                    const auto stop_typing = utils::scope_exit([&channel_manager, &message] {
                        channel_manager.stop_typing(message.jid);
                    });

                    // Stream relay: keep reasoning with the final answer, and only
                    // send tool cards when no approval card supersedes them.
                    const auto reply_target = resolve_reply_target(message);
                    const bool is_qq = reply_target.starts_with("qqbot:");
                    std::string thinking_buffer;
                    bool first_block_sent = false;
                    std::unordered_set<std::string> qq_progress_sent_tool_ids;

                    auto send_block = [&](const std::string &text) {
                        if (text.empty() || reply_target == "cli") {
                            return;
                        }
                        try {
                            channel_manager.send(reply_target, make_qq_stream_progress_message(text));
                            first_block_sent = true;
                        } catch (const std::exception &e) {
                            spdlog::debug("stream relay send failed: {}", e.what());
                        }
                    };

                    auto tool_progress_key = [](const ToolUse &call) {
                        return call.id.empty() ? fmt::format("{}:{}", call.name, call.input.dump()) : call.id;
                    };
                    auto send_tool_progress = [&](const ToolUse &call) {
                        const auto key = tool_progress_key(call);
                        if ((!call.id.empty() && qq_approval_prompted_tool_ids.contains(call.id)) || qq_progress_sent_tool_ids.contains(key)) {
                            return;
                        }
                        send_block(format_qq_tool_progress_markdown(call));
                        qq_progress_sent_tool_ids.insert(key);
                    };

                    AgentLoop::ProviderEventCallback stream_cb;
                    AgentLoop::ToolEventCallback tool_cb;
                    if (is_qq) {
                        stream_cb = [&](const ProviderEvent &event) {
                            if (const auto *thinking = std::get_if<ThinkingDelta>(&event)) {
                                thinking_buffer += thinking->thinking;
                            }
                        };
                        tool_cb = [&](const std::string &event_type, const ToolUse &call, const ToolResult *) {
                            if (event_type == "tool_started") {
                                if (should_send_qq_tool_progress_at_start(call, runtime.tools(), tool_context)) {
                                    send_tool_progress(call);
                                }
                            }
                        };
                    }

                    const auto reply = runtime.agent().run(build_agent_input(effective_message), stream_cb, tool_cb);

                    // Skip session persistence for isolated heartbeat-style inbound runs.
                    if (!message.isolated) {
                        persist_channel_session(message.jid, runtime, session_store);
                    }

                    // Suppress quiet heartbeat acknowledgements on heartbeat jid flows.
                    if (should_suppress_heartbeat_reply(message.jid, reply, heartbeat::DEFAULT_ACK_MAX_CHARS)) {
                        spdlog::debug("suppressing heartbeat acknowledgement from '{}'", message.jid);
                        return;
                    }

                    const auto qq_reply = is_qq ? format_qq_reply_markdown(thinking_buffer, reply) : reply;
                    if (first_block_sent && is_qq) {
                        // Blocks already streamed; keep the final answer tied to the inbound message.
                        if (!qq_reply.empty()) {
                            try {
                                channel_manager.send(reply_target, make_qq_stream_final_message(message, qq_reply));
                            } catch (const std::exception &e) {
                                spdlog::error("failed to deliver final reply for jid '{}': {}", message.jid, e.what());
                            }
                        }
                    } else {
                        deliver_reply(message, qq_reply, channel_manager);
                    }
                });
            } catch (const std::exception &e) {
                spdlog::error("failed to process message for jid '{}': {}", message.jid, e.what());
                deliver_reply(message, "Error: " + std::string(e.what()), channel_manager);
            } catch (...) {
                spdlog::error("failed to process message for jid '{}': unknown exception", message.jid);
                deliver_reply(message, "Error: internal failure while processing the message.", channel_manager);
            }
        }

    } // namespace

    std::size_t default_serve_worker_count() {
        const auto hardware = std::thread::hardware_concurrency();
        if (hardware == 0) {
            return 2;
        }
        return std::min<std::size_t>(2, hardware);
    }

    void run_channel_loop(MessageQueue &queue, ChannelManager &channel_manager, std::atomic<bool> &stop_requested, JidTaskRunner &task_runner,
                          const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const utils::transparent_string_unordered_map<std::string> &qq_bot_agents,
                          MemoryStore *memory_store, SessionStore &session_store, orchestration::OrchestrationManager *orchestration_manager, const Config &cfg,
                          HookManager *hook_manager, automation::AutomationRuntime *automation_runtime, orchestration::TeamManager *team_manager,
                          orchestration::AgentMailbox *mailbox) {
        std::unordered_map<std::string, std::unique_ptr<ConversationRuntime>> runtimes;
        ParkedResumeStateMap parked_resume_states;
        std::mutex runtimes_mutex;
        ChannelApprovalGate approval_gate;
        auto next_runtime_prune = std::chrono::steady_clock::now() + CHANNEL_RUNTIME_PRUNE_INTERVAL;

        while (!stop_requested.load()) {
            if (std::chrono::steady_clock::now() >= next_runtime_prune) {
                prune_inactive_runtimes(runtimes, parked_resume_states, orchestration_manager, runtimes_mutex);
                next_runtime_prune = std::chrono::steady_clock::now() + CHANNEL_RUNTIME_PRUNE_INTERVAL;
            }

            InboundMessage message;
            if (!queue.try_pop(message, SERVE_POLL_INTERVAL)) {
                if (queue.is_shutdown()) {
                    break;
                }
                continue;
            }

            if (message.jid.empty()) {
                continue;
            }

            if (!is_supported_inbound_event(message)) {
                spdlog::debug("ignoring unsupported inbound event for jid '{}'", message.jid);
                continue;
            }

            if (approval_gate.handle_inbound_message(message, channel_manager)) {
                continue;
            }

            task_runner.submit(message.jid, [message, &channel_manager, &runtimes, &parked_resume_states, &runtimes_mutex, &agent_configs, &qq_bot_agents, memory_store,
                                             &session_store, orchestration_manager, &cfg, hook_manager, automation_runtime, &approval_gate, &task_runner, team_manager, mailbox] {
                process_channel_message(message, channel_manager, runtimes, parked_resume_states, runtimes_mutex, agent_configs, qq_bot_agents, memory_store, session_store,
                                        orchestration_manager, cfg, hook_manager, automation_runtime, approval_gate, task_runner, team_manager, mailbox);
            });
        }

        approval_gate.shutdown();
        task_runner.shutdown(true);

        std::scoped_lock lock(runtimes_mutex);
        for (auto &[runtime_key, runtime] : runtimes) {
            static_cast<void>(runtime_key);
            dispatch_session_end(runtime->hook_manager, runtime->current_session_id, runtime->agent().history().size());
        }
        if (orchestration_manager != nullptr) {
            for (const auto &[runtime_key, parked_state] : parked_resume_states) {
                static_cast<void>(parked_state);
                orchestration_manager->unregister_runtime_notification_handler(runtime_key);
            }
        }
        runtimes.clear();
        parked_resume_states.clear();
    }

    void add_configured_channels(ChannelManager &channel_manager, const Config &cfg, utils::TaskPool &task_pool) {
        for (const auto &bot : cfg.qq_bots) {
            const bool has_qq_app_id = !bot.app_id.empty();
            const bool has_qq_secret = !bot.client_secret.empty();
            if (has_qq_app_id != has_qq_secret) {
                spdlog::warn("qq channel configuration is incomplete for bot '{}'; both app_id and client_secret are required",
                             bot.name.empty() ? std::string{"default"} : bot.name);
                continue;
            }
            if (!has_qq_app_id) {
                continue;
            }

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
            channel_manager.add_channel(std::make_unique<QqChannel>(bot.name, bot.app_id, bot.client_secret, task_pool));
#else
            spdlog::warn("qq credentials are configured, but this build was compiled without qq support. rebuild with -dorangutan_enable_qq_channel=on");
#endif
        }
    }

} // namespace orangutan::bootstrap
