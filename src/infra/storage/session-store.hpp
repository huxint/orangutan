#pragma once

#include "infra/storage/sqlite.hpp"
#include "core/types.hpp"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orangutan {

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
    std::string save(const std::vector<Message> &messages, const std::string &model, const std::string &scope_key = {});
    std::string save(const std::vector<Message> &messages, const SessionMetadata &metadata);

    // Create an empty session row before any messages exist, returns session ID
    std::string create_empty(const std::string &model, const std::string &scope_key = {});
    std::string create_empty(const SessionMetadata &metadata);

    // Update an existing session's messages (keeps same ID)
    void update(const std::string &session_id, const std::vector<Message> &messages, const std::string &model = {});
    void update(const std::string &session_id, const std::vector<Message> &messages, const SessionMetadata &metadata);

    // Append messages starting at start_index to an existing session
    void append(const std::string &session_id, const std::vector<Message> &messages, size_t start_index, const std::string &model = {});
    void append(const std::string &session_id, const std::vector<Message> &messages, size_t start_index, const SessionMetadata &metadata);

    // Load a session's messages by ID
    std::vector<Message> load(const std::string &session_id);

    // List all saved sessions
    std::vector<SessionInfo> list_sessions(const std::string &scope_key = {});
    std::vector<SessionInfo> list_sessions_for_agent(const std::string &agent_key);

    // Delete a session
    void remove(const std::string &session_id);

    // Get the ID of the most recently created session
    std::optional<std::string> latest_session_id();

    // Bind a channel JID to its current active session
    void bind_jid(const std::string &jid, const std::string &session_id, const std::string &agent_key = {});

    // Clear any active session binding for a channel JID
    void clear_jid(const std::string &jid, const std::string &agent_key = {});

    // Look up the current active session for a channel JID
    std::optional<std::string> bound_session_for_jid(const std::string &jid, const std::string &agent_key = {});

    [[nodiscard]]
    bool session_belongs_to_scope(const std::string &session_id, const std::string &scope_key);
    [[nodiscard]]
    bool session_belongs_to_agent(const std::string &session_id, const std::string &agent_key);

private:
    sqlite::Database db_;
    mutable std::mutex mutex_;

    void ensure_schema();
    static std::string generate_uuid();
    static SessionMetadata make_legacy_metadata(const std::string &model, const std::string &scope_key);
};

} // namespace orangutan
