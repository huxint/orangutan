#pragma once

#include "agent/agent-loop.hpp"
#include "automation/runtime.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/channel-serve-delivery.hpp"
#include "bootstrap/identity.hpp"
#include "bootstrap/runtime-assembler.hpp"
#include "cli/single-shot.hpp"
#include "config/config.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "permissions/permission-types.hpp"
#include "storage/session-store.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <chrono>

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig {
        std::string agent_key;
        std::string model;
        std::vector<std::string> fallback_models;
        providers::ProviderRoute provider_route;
        std::string api_key_override;
        std::string workspace_root;
        int thinking_budget = 0;
        std::string cli_runtime_key;
        std::string cli_memory_scope;
        Config::MemoryConfig memory;
        ToolPermissionContext permission_context;
        bool leader_mode = false;
        int max_concurrent_agents = 4;
    };

    struct ConversationRuntimeInspection {
        std::vector<ToolDef> tool_definitions;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id;
        bool has_agent = false;
        bool has_hook_manager = false;
        std::string session_scope_key;
        std::string configured_model;
        std::vector<std::string> fallback_models;
    };

    namespace detail {

        struct ChannelCompletionResumeState {
            std::mutex mutex;
            agent::AgentLoop *agent = nullptr;
            AgentRuntimeBundle *runtime = nullptr;
            providers::ProviderSystem *provider = nullptr;
            hooks::HookManager *hook_manager = nullptr;
            std::string *current_session_id = nullptr;
            std::size_t *persisted_message_count = nullptr;
            SessionStore *session_store = nullptr;
            ChannelManager *channel_manager = nullptr;
            std::string jid;
            std::string agent_key;
            std::string configured_model;
            std::string session_scope_key;
            const Config *app_config = nullptr;
            AgentRuntimeConfig runtime_config;
            MemoryStore *memory_store = nullptr;
            orchestration::OrchestrationManager *orchestration_manager = nullptr;
            hooks::HookManager *shared_hook_manager = nullptr;
            orchestration::TeamManager *team_manager = nullptr;
            orchestration::AgentMailbox *mailbox = nullptr;
            automation::AutomationRuntime *automation_runtime = nullptr;
            std::chrono::steady_clock::time_point parked_at{};
        };

        [[nodiscard]]
        BackgroundCompletionResumeCallback make_channel_completion_resume_callback(const std::weak_ptr<ChannelCompletionResumeState> &state);

        struct ConversationRuntime {
            std::unique_ptr<AgentRuntimeBundle> runtime;
            hooks::HookManager *hook_manager = nullptr;
            std::shared_ptr<ChannelCompletionResumeState> completion_resume_state;
            std::string runtime_key;
            std::string agent_key;
            std::string configured_model;
            providers::ProviderRoute provider_route;
            std::vector<std::string> fallback_models;
            std::string workspace;
            std::string memory_scope;
            std::string session_scope_key;
            std::string current_session_id;
            std::size_t persisted_message_count = 0;
            orchestration::OrchestrationManager *orchestration_manager = nullptr;
            bool owns_runtime_notification_handler = false;
            std::chrono::steady_clock::time_point last_used_at = std::chrono::steady_clock::now();
            std::size_t active_operations = 0;

            [[nodiscard]]
            providers::ProviderSystem *provider() const {
                return runtime != nullptr ? runtime->provider.get() : nullptr;
            }

            [[nodiscard]]
            ToolRegistry &tools() const {
                return runtime->tools();
            }

            [[nodiscard]]
            ToolRuntimeContext &tool_context() const {
                return runtime->tool_context();
            }

            [[nodiscard]]
            agent::AgentLoop &agent() const {
                return *runtime->agent;
            }

            ~ConversationRuntime() {
                if (owns_runtime_notification_handler && orchestration_manager != nullptr && !runtime_key.empty()) {
                    orchestration_manager->unregister_runtime_notification_handler(runtime_key);
                }
                if (completion_resume_state == nullptr) {
                    return;
                }
                std::scoped_lock lock(completion_resume_state->mutex);
                completion_resume_state->agent = nullptr;
                completion_resume_state->runtime = nullptr;
                completion_resume_state->provider = nullptr;
                completion_resume_state->hook_manager = nullptr;
                completion_resume_state->current_session_id = nullptr;
                completion_resume_state->persisted_message_count = nullptr;
            }
            ConversationRuntime() = default;
            ConversationRuntime(const ConversationRuntime &) = delete;
            ConversationRuntime &operator=(const ConversationRuntime &) = delete;
            ConversationRuntime(ConversationRuntime &&) = delete;
            ConversationRuntime &operator=(ConversationRuntime &&) = delete;
        };

        [[nodiscard]]
        inline std::unique_ptr<ConversationRuntime>
        make_conversation_runtime(const Config &app_cfg, const AgentRuntimeConfig &cfg, MemoryStore *memory_store, const RuntimeIdentity &identity,
                                  orchestration::OrchestrationManager *orchestration_manager, const std::string &raw_caller_id, hooks::HookManager *hook_manager,
                                  automation::AutomationRuntime *automation_runtime, orchestration::TeamManager *team_manager = nullptr,
                                  orchestration::AgentMailbox *mailbox = nullptr,
                                  std::shared_ptr<ChannelCompletionResumeState> completion_resume_state = nullptr,
                                  bool register_runtime_notification_handler = true) {
            auto runtime = std::make_unique<ConversationRuntime>();
            if (completion_resume_state == nullptr) {
                completion_resume_state = std::make_shared<ChannelCompletionResumeState>();
            }
            runtime->runtime_key = identity.runtime_key;
            runtime->agent_key = cfg.agent_key;
            runtime->configured_model = cfg.model;
            runtime->provider_route = cfg.provider_route;
            runtime->fallback_models = cfg.fallback_models;
            runtime->workspace = identity.workspace;
            runtime->memory_scope = identity.memory_scope;
            runtime->session_scope_key = identity.runtime_key.empty() ? cfg.cli_runtime_key : identity.runtime_key;
            runtime->orchestration_manager = orchestration_manager;
            completion_resume_state->jid = raw_caller_id;
            completion_resume_state->agent_key = cfg.agent_key;
            completion_resume_state->configured_model = cfg.model;
            completion_resume_state->session_scope_key = runtime->session_scope_key;
            completion_resume_state->app_config = &app_cfg;
            completion_resume_state->runtime_config = cfg;
            completion_resume_state->memory_store = memory_store;
            completion_resume_state->orchestration_manager = orchestration_manager;
            completion_resume_state->shared_hook_manager = hook_manager;
            completion_resume_state->team_manager = team_manager;
            completion_resume_state->mailbox = mailbox;
            completion_resume_state->automation_runtime = automation_runtime;
            completion_resume_state->parked_at = {};
            auto input = make_runtime_build_input(RuntimeAssemblyRequest{
                .runtime_config = &cfg,
                .identity = &identity,
                .app_config = &app_cfg,
                .memory_store = memory_store,
                .current_session_id = &runtime->current_session_id,
                .orchestration_manager = orchestration_manager,
                .team_manager = team_manager,
                .mailbox = mailbox,
                .runtime_origin = base::origin::channel,
                .raw_caller_id = raw_caller_id,
                .automation_service = automation_runtime != nullptr ? &automation_runtime->service() : nullptr,
                .automation_runtime = automation_runtime,
                .hook_manager = hook_manager,
                .background_completion_runtime = automation_runtime != nullptr ? make_background_completion_runtime_bindings(
                                                                                     [automation_runtime](const automation::DeliveryRecord &delivery) {
                                                                                         static_cast<void>(automation_runtime->service().record_delivery(delivery));
                                                                                     },
                                                                                     make_channel_completion_resume_callback(completion_resume_state))
                                                                               : nullptr,
            });
            runtime->runtime = std::make_unique<AgentRuntimeBundle>(build_agent_runtime(input));
            runtime->hook_manager = runtime->runtime->active_hook_manager();
            completion_resume_state->agent = runtime->runtime->agent.get();
            completion_resume_state->runtime = runtime->runtime.get();
            completion_resume_state->provider = runtime->runtime->provider.get();
            completion_resume_state->hook_manager = runtime->hook_manager;
            completion_resume_state->current_session_id = &runtime->current_session_id;
            completion_resume_state->persisted_message_count = &runtime->persisted_message_count;
            if (register_runtime_notification_handler && orchestration_manager != nullptr) {
                orchestration_manager->register_runtime_notification_handler(runtime->runtime_key, make_channel_completion_resume_callback(completion_resume_state));
            }
            runtime->owns_runtime_notification_handler = register_runtime_notification_handler;
            runtime->completion_resume_state = std::move(completion_resume_state);
            return runtime;
        }

        [[nodiscard]]
        inline std::shared_ptr<ChannelCompletionResumeState> park_conversation_runtime(ConversationRuntime &runtime) {
            auto state = std::move(runtime.completion_resume_state);
            runtime.owns_runtime_notification_handler = false;
            if (state == nullptr) {
                return {};
            }

            std::scoped_lock lock(state->mutex);
            state->agent = nullptr;
            state->runtime = nullptr;
            state->provider = nullptr;
            state->hook_manager = nullptr;
            state->current_session_id = nullptr;
            state->persisted_message_count = nullptr;
            state->parked_at = std::chrono::steady_clock::now();
            return state;
        }

        [[nodiscard]]
        inline SessionMetadata make_channel_session_metadata(const ConversationRuntime &runtime, const std::string &jid, const std::string &model) {
            return SessionMetadata{
                .model = model,
                .scope_key = runtime.session_scope_key,
                .agent_key = runtime.agent_key,
                .origin_kind = "channel",
                .origin_ref = jid,
            };
        }

        [[nodiscard]]
        inline SessionMetadata make_channel_session_metadata(const ChannelCompletionResumeState &state, const std::string &model) {
            return SessionMetadata{
                .model = model,
                .scope_key = state.session_scope_key,
                .agent_key = state.agent_key,
                .origin_kind = "channel",
                .origin_ref = state.jid,
            };
        }

        inline void rehydrate_session_permissions(SessionStore &session_store, std::string_view session_id, AgentRuntimeBundle *runtime) {
            if (runtime == nullptr) {
                return;
            }

            runtime->replace_permissions(session_store.load_session_permission_context(session_id, runtime->permissions()));
        }

        inline void persist_session_permissions(SessionStore &session_store, std::string_view session_id, const AgentRuntimeBundle *runtime) {
            if (runtime == nullptr || session_id.empty()) {
                return;
            }

            session_store.replace_session_permission_rules(session_id, runtime->permissions());
        }

        struct ChannelSessionPersistenceView {
            agent::AgentLoop &agent;
            providers::ProviderSystem *provider = nullptr;
            hooks::HookManager *hook_manager = nullptr;
            AgentRuntimeBundle *runtime = nullptr;
            std::string &current_session_id;
            std::size_t &persisted_message_count;
            std::string_view jid;
            std::string_view agent_key;
            std::string_view configured_model;
            std::string_view session_scope_key;
        };

        [[nodiscard]]
        inline ChannelSessionPersistenceView make_channel_session_persistence_view(ConversationRuntime &runtime, const std::string &jid) {
            return ChannelSessionPersistenceView{
                .agent = runtime.agent(),
                .provider = runtime.provider(),
                .hook_manager = runtime.hook_manager,
                .runtime = runtime.runtime.get(),
                .current_session_id = runtime.current_session_id,
                .persisted_message_count = runtime.persisted_message_count,
                .jid = jid,
                .agent_key = runtime.agent_key,
                .configured_model = runtime.configured_model,
                .session_scope_key = runtime.session_scope_key,
            };
        }

        [[nodiscard]]
        inline std::optional<ChannelSessionPersistenceView> make_channel_session_persistence_view(ChannelCompletionResumeState &state) {
            if (state.agent == nullptr || state.current_session_id == nullptr || state.persisted_message_count == nullptr) {
                return std::nullopt;
            }

            return ChannelSessionPersistenceView{
                .agent = *state.agent,
                .provider = state.provider,
                .hook_manager = state.hook_manager,
                .runtime = state.runtime,
                .current_session_id = *state.current_session_id,
                .persisted_message_count = *state.persisted_message_count,
                .jid = state.jid,
                .agent_key = state.agent_key,
                .configured_model = state.configured_model,
                .session_scope_key = state.session_scope_key,
            };
        }

        inline void persist_channel_session(ChannelSessionPersistenceView view, SessionStore &session_store) {
            const auto &history = view.agent.history();
            if (history.empty()) {
                session_store.clear_jid(view.jid, view.agent_key);
                view.current_session_id.clear();
                rehydrate_session_permissions(session_store, view.current_session_id, view.runtime);
                view.persisted_message_count = 0;
                return;
            }

            const bool created_session = view.current_session_id.empty();
            const auto active_model = view.provider != nullptr && view.provider->active_target().has_value() ? view.provider->active_target()->model
                                                                                                            : std::string(view.configured_model);
            const auto metadata = SessionMetadata{
                .model = active_model,
                .scope_key = std::string(view.session_scope_key),
                .agent_key = std::string(view.agent_key),
                .origin_kind = "channel",
                .origin_ref = std::string(view.jid),
            };
            if (view.current_session_id.empty()) {
                view.current_session_id = session_store.save(history, metadata);
                view.persisted_message_count = history.size();
            } else if (history.size() > view.persisted_message_count) {
                session_store.append(view.current_session_id, history, view.persisted_message_count, metadata);
                view.persisted_message_count = history.size();
            } else {
                session_store.update(view.current_session_id, history, metadata);
                view.persisted_message_count = history.size();
            }

            persist_session_permissions(session_store, view.current_session_id, view.runtime);
            session_store.bind_jid(view.jid, view.current_session_id, view.agent_key);
            if (created_session) {
                dispatch_session_start(view.hook_manager, view.current_session_id, history.size());
            }
        }

        inline void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store) {
            persist_channel_session(make_channel_session_persistence_view(runtime, jid), session_store);
        }

        inline std::optional<std::string> persist_channel_session(ChannelCompletionResumeState &state) {
            if (state.session_store == nullptr) {
                return "channel runtime is no longer available";
            }

            auto view = make_channel_session_persistence_view(state);
            if (!view.has_value()) {
                return "channel runtime is no longer available";
            }

            persist_channel_session(*view, *state.session_store);
            return std::nullopt;
        }

        inline void restore_bound_channel_session(SessionStore &session_store, const std::string &jid, ConversationRuntime &runtime) {
            if (auto session_id = session_store.bound_session_for_jid(jid, runtime.agent_key); session_id.has_value()) {
                try {
                    if (!session_store.session_belongs_to_scope(*session_id, runtime.session_scope_key)) {
                        spdlog::warn("session {} does not belong to runtime scope '{}' for jid '{}' agent '{}'", *session_id, runtime.session_scope_key, jid,
                                     runtime.agent_key);
                        session_store.clear_jid(jid, runtime.agent_key);
                        rehydrate_session_permissions(session_store, {}, runtime.runtime.get());
                        return;
                    }

                    rehydrate_session_permissions(session_store, *session_id, runtime.runtime.get());
                    runtime.agent().set_history(session_store.load(*session_id));
                    runtime.current_session_id = *session_id;
                    runtime.persisted_message_count = runtime.agent().history().size();
                    spdlog::info("restored session {} for jid '{}' agent '{}'", *session_id, jid, runtime.agent_key);
                    dispatch_session_start(runtime.hook_manager, runtime.current_session_id, runtime.agent().history().size());
                } catch (const std::exception &e) {
                    spdlog::warn("failed to restore session {} for jid '{}' agent '{}': {}", *session_id, jid, runtime.agent_key, e.what());
                    session_store.clear_jid(jid, runtime.agent_key);
                    rehydrate_session_permissions(session_store, {}, runtime.runtime.get());
                }
            }
        }

        [[nodiscard]]
        inline std::unique_ptr<ConversationRuntime> rebuild_conversation_runtime(ChannelCompletionResumeState &state,
                                                                                 const std::shared_ptr<ChannelCompletionResumeState> &state_ref) {
            if (state.app_config == nullptr || state.memory_store == nullptr) {
                return nullptr;
            }

            const auto identity = derive_channel_identity(state.runtime_config.workspace_root, state.jid, state.agent_key);
            auto runtime = make_conversation_runtime(*state.app_config, state.runtime_config, state.memory_store, identity, state.orchestration_manager, state.jid,
                                                     state.shared_hook_manager, state.automation_runtime, state.team_manager, state.mailbox, state_ref, false);
            if (state.session_store != nullptr) {
                restore_bound_channel_session(*state.session_store, state.jid, *runtime);
            }
            return runtime;
        }

        [[nodiscard]]
        inline ConversationRuntimeInspection inspect_conversation_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg, MemoryStore *memory_store,
                                                                          orchestration::OrchestrationManager *orchestration_manager, const std::string &raw_caller_id,
                                                                          hooks::HookManager *hook_manager = nullptr,
                                                                          automation::AutomationRuntime *automation_runtime = nullptr,
                                                                          orchestration::TeamManager *team_manager = nullptr, orchestration::AgentMailbox *mailbox = nullptr) {
            const auto identity = derive_channel_identity(runtime_cfg.workspace_root, raw_caller_id, runtime_cfg.agent_key);
            auto runtime =
                make_conversation_runtime(cfg, runtime_cfg, memory_store, identity, orchestration_manager, raw_caller_id, hook_manager, automation_runtime, team_manager, mailbox);

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

        [[nodiscard]]
        inline BackgroundCompletionResumeCallback make_channel_completion_resume_callback(const std::weak_ptr<ChannelCompletionResumeState> &weak_state) {
            return [weak_state](const std::string &message) -> std::optional<std::string> {
                const auto state = weak_state.lock();
                if (!state) {
                    return "channel runtime is no longer available";
                }

                std::scoped_lock lock(state->mutex);
                std::unique_ptr<ConversationRuntime> rebuilt_runtime;
                if (state->agent == nullptr) {
                    rebuilt_runtime = rebuild_conversation_runtime(*state, state);
                    if (rebuilt_runtime == nullptr || state->agent == nullptr) {
                        return "channel runtime is no longer available";
                    }
                }

                return cli::run_completion_resume_message(
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

} // namespace orangutan::bootstrap
