#include "bootstrap/channel-session-persistence.hpp"

#include "agent/agent-loop.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/channel-serve-delivery.hpp"
#include "bootstrap/channel-serve-runtime.hpp"
#include "hooks/hook-manager.hpp"
#include "providers/provider.hpp"

#include <exception>

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap::detail {

    SessionMetadata make_channel_session_metadata(const ConversationRuntime &runtime, const std::string &jid, const std::string &model) {
        return SessionMetadata{
            .model = model,
            .scope_key = runtime.session_scope_key,
            .agent_key = runtime.agent_key,
            .origin_kind = "channel",
            .origin_ref = jid,
        };
    }

    SessionMetadata make_channel_session_metadata(const ChannelCompletionResumeState &state, const std::string &model) {
        return SessionMetadata{
            .model = model,
            .scope_key = state.session_scope_key,
            .agent_key = state.agent_key,
            .origin_kind = "channel",
            .origin_ref = state.jid,
        };
    }

    void rehydrate_session_permissions(SessionStore &session_store, std::string_view session_id, AgentRuntimeBundle *runtime) {
        if (runtime == nullptr) {
            return;
        }

        runtime->replace_permissions(session_store.load_session_permission_context(session_id, runtime->permissions()));
    }

    void persist_session_permissions(SessionStore &session_store, std::string_view session_id, const AgentRuntimeBundle *runtime) {
        if (runtime == nullptr || session_id.empty()) {
            return;
        }

        session_store.replace_session_permission_rules(session_id, runtime->permissions());
    }

    ChannelSessionPersistenceView make_channel_session_persistence_view(ConversationRuntime &runtime, const std::string &jid) {
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

    std::optional<ChannelSessionPersistenceView> make_channel_session_persistence_view(ChannelCompletionResumeState &state) {
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

    void persist_channel_session(ChannelSessionPersistenceView view, SessionStore &session_store) {
        const auto &history = view.agent.history();
        if (history.empty()) {
            session_store.clear_jid(view.jid, view.agent_key);
            view.current_session_id.clear();
            rehydrate_session_permissions(session_store, view.current_session_id, view.runtime);
            view.persisted_message_count = 0;
            return;
        }

        const bool created_session = view.current_session_id.empty();
        const auto active_model =
            view.provider != nullptr && view.provider->active_target().has_value() ? view.provider->active_target()->model : std::string(view.configured_model);
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

    void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store) {
        persist_channel_session(make_channel_session_persistence_view(runtime, jid), session_store);
    }

    std::optional<std::string> persist_channel_session(ChannelCompletionResumeState &state) {
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

    void restore_bound_channel_session(SessionStore &session_store, const std::string &jid, ConversationRuntime &runtime) {
        if (auto session_id = session_store.bound_session_for_jid(jid, runtime.agent_key); session_id.has_value()) {
            try {
                if (!session_store.session_belongs_to_scope(*session_id, runtime.session_scope_key)) {
                    spdlog::warn("session {} does not belong to runtime scope '{}' for jid '{}' agent '{}'", *session_id, runtime.session_scope_key, jid, runtime.agent_key);
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

} // namespace orangutan::bootstrap::detail
