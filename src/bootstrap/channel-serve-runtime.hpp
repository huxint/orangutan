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

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig {
        std::string agent_key;
        std::string model;
        std::vector<std::string> fallback_models;
        providers::ProviderRoute provider_route;
        std::string workspace_root;
        std::string edit_mode = "hashline";
        int thinking_budget = 0;
        std::string cli_runtime_key;
        std::string cli_memory_scope;
        Config::MemoryConfig memory;
        ToolPermissionContext permission_context;
        std::vector<std::string> team_agents;
        bool coordinator_mode = false;
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
            automation::AutomationRuntime *automation_runtime = nullptr;
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
            std::vector<std::string> fallback_models;
            std::string workspace;
            std::string memory_scope;
            std::string session_scope_key;
            std::string current_session_id;
            std::size_t persisted_message_count = 0;

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

        [[nodiscard]]
        inline std::unique_ptr<ConversationRuntime>
        make_conversation_runtime(const Config &app_cfg, const AgentRuntimeConfig &cfg, MemoryStore *memory_store, const RuntimeIdentity &identity,
                                  orchestration::OrchestrationManager *orchestration_manager, const std::string &raw_caller_id, hooks::HookManager *hook_manager,
                                  automation::AutomationRuntime *automation_runtime, orchestration::TeamManager *team_manager = nullptr,
                                  orchestration::AgentMailbox *mailbox = nullptr) {
            auto runtime = std::make_unique<ConversationRuntime>();
            auto completion_resume_state = std::make_shared<ChannelCompletionResumeState>();
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
                .background_completion_runtime = automation_runtime != nullptr ? make_background_completion_runtime_bindings(
                                                                                     [automation_runtime](const automation::DeliveryRecord &delivery) {
                                                                                         static_cast<void>(automation_runtime->service().record_delivery(delivery));
                                                                                     },
                                                                                     make_channel_completion_resume_callback(completion_resume_state))
                                                                               : nullptr,
            });
            runtime->runtime = std::make_unique<AgentRuntimeBundle>(build_agent_runtime(input));
            if (hook_manager != nullptr) {
                runtime->runtime->agent = std::make_unique<agent::AgentLoop>(*runtime->runtime->provider, cfg.provider_route, runtime->runtime->tools(),
                                                                             runtime->runtime->memory.get(), runtime->runtime->skills_prompt, hook_manager,
                                                                             runtime->runtime->skill_loader.get());
                runtime->hook_manager = hook_manager;
            } else {
                runtime->hook_manager = runtime->runtime->hook_manager.get();
            }
            completion_resume_state->agent = runtime->runtime->agent.get();
            completion_resume_state->runtime = runtime->runtime.get();
            completion_resume_state->provider = runtime->runtime->provider.get();
            completion_resume_state->hook_manager = runtime->hook_manager;
            completion_resume_state->current_session_id = &runtime->current_session_id;
            completion_resume_state->persisted_message_count = &runtime->persisted_message_count;
            if (orchestration_manager != nullptr) {
                orchestration_manager->register_runtime_notification_handler(runtime->runtime_key, make_channel_completion_resume_callback(completion_resume_state));
            }
            runtime->completion_resume_state = std::move(completion_resume_state);
            return runtime;
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

        inline void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store) {
            const auto &history = runtime.agent().history();
            if (history.empty()) {
                session_store.clear_jid(jid, runtime.agent_key);
                runtime.current_session_id.clear();
                rehydrate_session_permissions(session_store, runtime.current_session_id, runtime.runtime.get());
                runtime.persisted_message_count = 0;
                return;
            }

            const bool created_session = runtime.current_session_id.empty();
            const auto active_model =
                runtime.provider() != nullptr && runtime.provider()->active_target().has_value() ? runtime.provider()->active_target()->model : runtime.configured_model;
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

            persist_session_permissions(session_store, runtime.current_session_id, runtime.runtime.get());
            session_store.bind_jid(jid, runtime.current_session_id, runtime.agent_key);
            if (created_session) {
                dispatch_session_start(runtime.hook_manager, runtime.current_session_id, history.size());
            }
        }

        inline std::optional<std::string> persist_channel_session(ChannelCompletionResumeState &state) {
            if (state.agent == nullptr || state.current_session_id == nullptr || state.persisted_message_count == nullptr || state.session_store == nullptr) {
                return "channel runtime is no longer available";
            }

            const auto &history = state.agent->history();
            if (history.empty()) {
                state.session_store->clear_jid(state.jid, state.agent_key);
                state.current_session_id->clear();
                rehydrate_session_permissions(*state.session_store, *state.current_session_id, state.runtime);
                *state.persisted_message_count = 0;
                return std::nullopt;
            }

            const bool created_session = state.current_session_id->empty();
            const auto active_model = state.provider != nullptr && state.provider->active_target().has_value() ? state.provider->active_target()->model : state.configured_model;
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

            persist_session_permissions(*state.session_store, *state.current_session_id, state.runtime);
            state.session_store->bind_jid(state.jid, *state.current_session_id, state.agent_key);
            if (created_session) {
                dispatch_session_start(state.hook_manager, *state.current_session_id, history.size());
            }
            return std::nullopt;
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
                if (state->agent == nullptr) {
                    return "channel runtime is no longer available";
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
