#pragma once

#include "permissions/permission-types.hpp"
#include "storage/sqlite.hpp"
#include "types/types.hpp"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::storage {

    struct SessionMetadata {
        std::string model;
        std::string scope_key;
        std::string agent_key;
        std::string origin_kind;
        std::string origin_ref;
    };

    struct SessionInfo {
        std::string id;
        std::string created_at;
        std::string model;
        std::string scope_key;
        std::string agent_key;
        std::string origin_kind;
        std::string origin_ref;
        int message_count = 0;
    };

    class SessionStore {
    public:
        // Uses default path: ~/.orangutan/sessions.db
        SessionStore();

        // Use a specific database path (for testing)
        explicit SessionStore(const std::filesystem::path &db_path);

        ~SessionStore();

        SessionStore(const SessionStore &) = delete;
        SessionStore &operator=(const SessionStore &) = delete;
        SessionStore(SessionStore &&) = delete;
        SessionStore &operator=(SessionStore &&) = delete;

        // Save current conversation as a new session, returns session ID
        std::string save(const std::vector<Message> &messages, const SessionMetadata &metadata);

        // Create an empty session row before any messages exist, returns session ID
        std::string create_empty(const SessionMetadata &metadata);

        // Update an existing session's messages (keeps same ID)
        void update(std::string_view session_id, const std::vector<Message> &messages, std::string_view model = {});
        void update(std::string_view session_id, const std::vector<Message> &messages, const SessionMetadata &metadata);

        // Append messages starting at start_index to an existing session
        void append(std::string_view session_id, const std::vector<Message> &messages, std::size_t start_index, std::string_view model = {});
        void append(std::string_view session_id, const std::vector<Message> &messages, std::size_t start_index, const SessionMetadata &metadata);

        // Load a session's messages by ID
        std::vector<Message> load(std::string_view session_id);

        // Persist/load session-scoped permission rules that should be rehydrated when a session resumes.
        void save_session_permission_rule(std::string_view session_id, PermissionRule rule);
        std::vector<PermissionRule> load_session_permission_rules(std::string_view session_id);
        ToolPermissionContext load_session_permission_context(std::string_view session_id, const ToolPermissionContext &base_context);
        void replace_session_permission_rules(std::string_view session_id, const ToolPermissionContext &context);
        void clear_session_permission_rules(std::string_view session_id);

        // List all saved sessions
        std::vector<SessionInfo> list_sessions(std::string_view scope_key = {});
        std::vector<SessionInfo> list_sessions_for_agent(std::string_view agent_key);

        // Delete a session
        void remove(std::string_view session_id);

        // Get the ID of the most recently created session
        std::optional<std::string> latest_session_id();

        // Bind a channel JID to its current active session
        void bind_jid(std::string_view jid, std::string_view session_id, std::string_view agent_key = {});

        // Clear any active session binding for a channel JID
        void clear_jid(std::string_view jid, std::string_view agent_key = {});

        // Look up the current active session for a channel JID
        std::optional<std::string> bound_session_for_jid(std::string_view jid, std::string_view agent_key = {});

        [[nodiscard]]
        bool session_belongs_to_scope(std::string_view session_id, std::string_view scope_key);
        [[nodiscard]]
        bool session_belongs_to_agent(std::string_view session_id, std::string_view agent_key);

    private:
        sqlite::Database db_;
        mutable std::mutex mutex_;

        void ensure_schema();
        static std::string generate_uuid();
    };

} // namespace orangutan::storage

namespace orangutan {

    using storage::SessionInfo;
    using storage::SessionMetadata;
    using storage::SessionStore;

} // namespace orangutan
