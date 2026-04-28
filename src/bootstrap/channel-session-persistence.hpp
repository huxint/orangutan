#pragma once

#include "storage/session-store.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace orangutan::agent {
    class AgentLoop;
}

namespace orangutan::providers {
    class ProviderSystem;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::bootstrap {

    struct AgentRuntimeBundle;

    namespace detail {

        struct ChannelCompletionResumeState;
        struct ConversationRuntime;

        [[nodiscard]]
        SessionMetadata make_channel_session_metadata(const ConversationRuntime &runtime, const std::string &jid, const std::string &model);

        [[nodiscard]]
        SessionMetadata make_channel_session_metadata(const ChannelCompletionResumeState &state, const std::string &model);

        void rehydrate_session_permissions(SessionStore &session_store, std::string_view session_id, AgentRuntimeBundle *runtime);

        void persist_session_permissions(SessionStore &session_store, std::string_view session_id, const AgentRuntimeBundle *runtime);

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
        ChannelSessionPersistenceView make_channel_session_persistence_view(ConversationRuntime &runtime, const std::string &jid);

        [[nodiscard]]
        std::optional<ChannelSessionPersistenceView> make_channel_session_persistence_view(ChannelCompletionResumeState &state);

        void persist_channel_session(ChannelSessionPersistenceView view, SessionStore &session_store);

        void persist_channel_session(const std::string &jid, ConversationRuntime &runtime, SessionStore &session_store);

        [[nodiscard]]
        std::optional<std::string> persist_channel_session(ChannelCompletionResumeState &state);

        void restore_bound_channel_session(SessionStore &session_store, const std::string &jid, ConversationRuntime &runtime);

    } // namespace detail

} // namespace orangutan::bootstrap
