#include "infra/storage/session-store.hpp"

#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <spdlog/spdlog.h>

namespace orangutan {

namespace {

std::string db_path() {
    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        return "orangutan_sessions.db";
    }

    return (std::filesystem::path(home) / ".orangutan" / "sessions.db").string();
}

json serialize_content(const std::vector<ContentBlock> &content) {
    auto arr = json::array();
    for (const auto &block : content) {
        arr.push_back(content_block_to_json(block));
    }
    return arr;
}

std::vector<ContentBlock> deserialize_content(const std::string &json_str) {
    std::vector<ContentBlock> blocks;
    const auto arr = json::parse(json_str);

    for (const auto &item : arr) {
        const auto type = item.at("type").get<std::string>();
        if (type == "text") {
            blocks.emplace_back(TextBlock{.text = item.at("text").get<std::string>()});
            continue;
        }
        if (type == "tool_use") {
            blocks.emplace_back(ToolUseBlock{.id = item.at("id").get<std::string>(), .name = item.at("name").get<std::string>(), .input = item.at("input")});
            continue;
        }
        if (type == "tool_result") {
            blocks.emplace_back(ToolResultBlock{
                .tool_use_id = item.at("tool_use_id").get<std::string>(),
                .content = item.at("content").get<std::string>(),
                .is_error = item.value("is_error", false),
            });
        }
    }

    return blocks;
}

void ensure_column(sqlite::Database &db, std::string_view table_name, std::string_view column_name, std::string_view add_sql) {
    sqlite::Statement pragma(db, std::string("PRAGMA table_info(") + std::string(table_name) + ")");
    while (pragma.step()) {
        if (pragma.column_text(1) == column_name) {
            return;
        }
    }

    db.exec(add_sql, "Failed to migrate session schema");
}

struct ChannelBindingSchema {
    bool has_agent_key = false;
    int jid_pk_position = 0;
    int agent_key_pk_position = 0;
};

ChannelBindingSchema inspect_channel_binding_schema(sqlite::Database &db) {
    sqlite::Statement pragma(db, "PRAGMA table_info(channel_session_bindings)");

    ChannelBindingSchema schema;
    while (pragma.step()) {
        const auto column_name = pragma.column_text(1);
        const auto pk_position = pragma.column_int(5);

        if (column_name == "jid") {
            schema.jid_pk_position = pk_position;
        }
        if (column_name == "agent_key") {
            schema.has_agent_key = true;
            schema.agent_key_pk_position = pk_position;
        }
    }

    return schema;
}

void rebuild_channel_binding_table(sqlite::Database &db, bool legacy_has_agent_key) {
    db.exec("DROP INDEX IF EXISTS idx_channel_session_bindings_updated;", "Failed to drop old binding index");
    db.exec("ALTER TABLE channel_session_bindings RENAME TO channel_session_bindings_legacy;", "Failed to rename legacy binding table");
    db.exec("CREATE TABLE channel_session_bindings ("
            "jid TEXT NOT NULL,"
            "agent_key TEXT NOT NULL DEFAULT '',"
            "session_id TEXT NOT NULL,"
            "updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
            "PRIMARY KEY (jid, agent_key),"
            "FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE"
            ");",
            "Failed to create upgraded binding table");

    db.exec(legacy_has_agent_key ? "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                                   "SELECT jid, COALESCE(agent_key, ''), session_id, updated_at FROM channel_session_bindings_legacy;"
                                 : "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                                   "SELECT jid, '', session_id, updated_at FROM channel_session_bindings_legacy;",
            "Failed to migrate legacy channel bindings");
    db.exec("DROP TABLE channel_session_bindings_legacy;", "Failed to drop legacy binding table");
    db.exec("CREATE INDEX IF NOT EXISTS idx_channel_session_bindings_updated ON channel_session_bindings(updated_at);", "Failed to recreate binding index");
}

void write_messages(sqlite::Database &db, const std::string &session_id, const std::vector<Message> &messages, size_t start_index) {
    sqlite::Statement insert_msg(db, "INSERT INTO messages (session_id, seq, role, content_json) VALUES (?, ?, ?, ?)");
    for (size_t index = start_index; index < messages.size(); ++index) {
        const auto &message = messages[index];
        const auto content_json = serialize_content(message.content).dump();

        insert_msg.bind_text(1, session_id);
        insert_msg.bind_int(2, static_cast<int>(index));
        insert_msg.bind_text(3, message.role);
        insert_msg.bind_text(4, content_json);
        static_cast<void>(insert_msg.step());
        insert_msg.reset();
    }
}

void insert_session(sqlite::Database &db, const std::string &session_id, const SessionMetadata &metadata) {
    sqlite::Statement insert_session_stmt(db, "INSERT INTO sessions (id, model, scope_key, agent_key, origin_kind, origin_ref) "
                                              "VALUES (?, ?, ?, ?, ?, ?)");
    insert_session_stmt.bind_text(1, session_id);
    insert_session_stmt.bind_text(2, metadata.model);
    insert_session_stmt.bind_text(3, metadata.scope_key);
    insert_session_stmt.bind_text(4, metadata.agent_key);
    insert_session_stmt.bind_text(5, metadata.origin_kind);
    insert_session_stmt.bind_text(6, metadata.origin_ref);
    static_cast<void>(insert_session_stmt.step());
}

void update_session_model(sqlite::Database &db, const std::string &session_id, const std::string &model) {
    if (model.empty()) {
        return;
    }

    sqlite::Statement stmt(db, "UPDATE sessions SET model = ? WHERE id = ?");
    stmt.bind_text(1, model);
    stmt.bind_text(2, session_id);
    static_cast<void>(stmt.step());
}

void update_session_metadata(sqlite::Database &db, const std::string &session_id, const SessionMetadata &metadata) {
    sqlite::Statement stmt(db, "UPDATE sessions "
                               "SET model = ?, scope_key = ?, agent_key = ?, origin_kind = ?, origin_ref = ? "
                               "WHERE id = ?");
    stmt.bind_text(1, metadata.model);
    stmt.bind_text(2, metadata.scope_key);
    stmt.bind_text(3, metadata.agent_key);
    stmt.bind_text(4, metadata.origin_kind);
    stmt.bind_text(5, metadata.origin_ref);
    stmt.bind_text(6, session_id);
    static_cast<void>(stmt.step());
}

bool session_exists(sqlite::Database &db, const std::string &session_id) {
    sqlite::Statement stmt(db, "SELECT 1 FROM sessions WHERE id = ? LIMIT 1");
    stmt.bind_text(1, session_id);
    return stmt.step();
}

} // namespace

SessionStore::SessionStore()
: db_(db_path()) {
    db_.exec("PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
    ensure_schema();
}

SessionStore::SessionStore(const std::string &path)
: db_(path) {
    db_.exec("PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
    ensure_schema();
}

SessionStore::~SessionStore() = default;

void SessionStore::ensure_schema() {
    db_.exec(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id TEXT PRIMARY KEY,
            model TEXT NOT NULL,
            scope_key TEXT NOT NULL DEFAULT '',
            agent_key TEXT NOT NULL DEFAULT '',
            origin_kind TEXT NOT NULL DEFAULT 'cli',
            origin_ref TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT NOT NULL,
            seq INTEGER NOT NULL,
            role TEXT NOT NULL,
            content_json TEXT NOT NULL,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS channel_session_bindings (
            jid TEXT NOT NULL,
            agent_key TEXT NOT NULL DEFAULT '',
            session_id TEXT NOT NULL,
            updated_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (jid, agent_key),
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq);
        CREATE INDEX IF NOT EXISTS idx_channel_session_bindings_updated ON channel_session_bindings(updated_at);
    )",
             "Failed to create session schema");

    ensure_column(db_, "sessions", "scope_key", "ALTER TABLE sessions ADD COLUMN scope_key TEXT NOT NULL DEFAULT ''");
    ensure_column(db_, "sessions", "agent_key", "ALTER TABLE sessions ADD COLUMN agent_key TEXT NOT NULL DEFAULT ''");
    ensure_column(db_, "sessions", "origin_kind", "ALTER TABLE sessions ADD COLUMN origin_kind TEXT NOT NULL DEFAULT 'cli'");
    ensure_column(db_, "sessions", "origin_ref", "ALTER TABLE sessions ADD COLUMN origin_ref TEXT NOT NULL DEFAULT ''");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_sessions_scope_key ON sessions(scope_key, created_at DESC);", "Failed to create session scope index");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_sessions_agent_key ON sessions(agent_key, created_at DESC);", "Failed to create session agent index");

    const auto binding_schema = inspect_channel_binding_schema(db_);
    if (!binding_schema.has_agent_key || binding_schema.jid_pk_position != 1 || binding_schema.agent_key_pk_position != 2) {
        sqlite::Transaction tx(db_);
        rebuild_channel_binding_table(db_, binding_schema.has_agent_key);
        tx.commit();
    }
}

std::string SessionStore::generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;

    std::ostringstream out;
    out << std::hex;
    out << dist(gen) << '-';
    out << (dist(gen) & 0xFFFF) << '-';
    out << (0x4000 | (dist(gen) & 0x0FFF)) << '-';
    out << (0x8000 | (dist(gen) & 0x3FFF)) << '-';
    out << dist(gen) << (dist(gen) & 0xFFFF);
    return out.str();
}

std::string SessionStore::save(const std::vector<Message> &messages, const std::string &model, const std::string &scope_key) {
    return save(messages, make_legacy_metadata(model, scope_key));
}

std::string SessionStore::save(const std::vector<Message> &messages, const SessionMetadata &metadata) {
    std::scoped_lock lock(mutex_);
    const auto session_id = generate_uuid();
    sqlite::Transaction tx(db_);

    insert_session(db_, session_id, metadata);

    write_messages(db_, session_id, messages, 0);

    tx.commit();
    spdlog::info("Saved session {} ({} messages)", session_id, messages.size());
    return session_id;
}

std::string SessionStore::create_empty(const std::string &model, const std::string &scope_key) {
    return create_empty(make_legacy_metadata(model, scope_key));
}

std::string SessionStore::create_empty(const SessionMetadata &metadata) {
    std::scoped_lock lock(mutex_);
    const auto session_id = generate_uuid();
    sqlite::Transaction tx(db_);

    insert_session(db_, session_id, metadata);

    tx.commit();
    spdlog::info("Created empty session {}", session_id);
    return session_id;
}

void SessionStore::update(const std::string &session_id, const std::vector<Message> &messages, const std::string &model) {
    std::scoped_lock lock(mutex_);
    sqlite::Transaction tx(db_);

    update_session_model(db_, session_id, model);

    sqlite::Statement delete_messages(db_, "DELETE FROM messages WHERE session_id = ?");
    delete_messages.bind_text(1, session_id);
    static_cast<void>(delete_messages.step());

    write_messages(db_, session_id, messages, 0);

    tx.commit();
    spdlog::info("Updated session {} ({} messages)", session_id, messages.size());
}

void SessionStore::update(const std::string &session_id, const std::vector<Message> &messages, const SessionMetadata &metadata) {
    std::scoped_lock lock(mutex_);
    sqlite::Transaction tx(db_);

    update_session_metadata(db_, session_id, metadata);

    sqlite::Statement delete_messages(db_, "DELETE FROM messages WHERE session_id = ?");
    delete_messages.bind_text(1, session_id);
    static_cast<void>(delete_messages.step());

    write_messages(db_, session_id, messages, 0);

    tx.commit();
    spdlog::info("Updated session {} ({} messages)", session_id, messages.size());
}

void SessionStore::append(const std::string &session_id, const std::vector<Message> &messages, size_t start_index, const std::string &model) {
    std::scoped_lock lock(mutex_);
    if (start_index > messages.size()) {
        throw std::runtime_error("append start_index is out of range");
    }

    sqlite::Transaction tx(db_);
    update_session_model(db_, session_id, model);
    write_messages(db_, session_id, messages, start_index);
    tx.commit();

    spdlog::info("Appended {} message(s) to session {}", messages.size() - start_index, session_id);
}

void SessionStore::append(const std::string &session_id, const std::vector<Message> &messages, size_t start_index, const SessionMetadata &metadata) {
    std::scoped_lock lock(mutex_);
    if (start_index > messages.size()) {
        throw std::runtime_error("append start_index is out of range");
    }

    sqlite::Transaction tx(db_);
    update_session_metadata(db_, session_id, metadata);
    write_messages(db_, session_id, messages, start_index);
    tx.commit();

    spdlog::info("Appended {} message(s) to session {}", messages.size() - start_index, session_id);
}

std::vector<Message> SessionStore::load(const std::string &session_id) {
    std::scoped_lock lock(mutex_);

    sqlite::Statement stmt(db_, "SELECT role, content_json FROM messages WHERE session_id = ? ORDER BY seq");
    stmt.bind_text(1, session_id);

    std::vector<Message> messages;
    while (stmt.step()) {
        messages.push_back({
            .role = stmt.column_text(0),
            .content = deserialize_content(stmt.column_text(1)),
        });
    }

    if (messages.empty() && !session_exists(db_, session_id)) {
        throw std::runtime_error("Session not found: " + session_id);
    }

    return messages;
}

std::vector<SessionInfo> SessionStore::list_sessions(const std::string &scope_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, scope_key.empty() ? "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
                                                    "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
                                                    "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC"
                                                  : "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
                                                    "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
                                                    "WHERE s.scope_key = ? "
                                                    "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC");

    if (!scope_key.empty()) {
        stmt.bind_text(1, scope_key);
    }

    std::vector<SessionInfo> sessions;
    while (stmt.step()) {
        sessions.push_back({
            .id = stmt.column_text(0),
            .created_at = stmt.column_text(1),
            .model = stmt.column_text(2),
            .scope_key = stmt.column_text(3),
            .agent_key = stmt.column_text(4),
            .origin_kind = stmt.column_text(5),
            .origin_ref = stmt.column_text(6),
            .message_count = stmt.column_int(7),
        });
    }
    return sessions;
}

std::vector<SessionInfo> SessionStore::list_sessions_for_agent(const std::string &agent_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
                                "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
                                "WHERE s.agent_key = ? "
                                "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC");
    stmt.bind_text(1, agent_key);

    std::vector<SessionInfo> sessions;
    while (stmt.step()) {
        sessions.push_back({
            .id = stmt.column_text(0),
            .created_at = stmt.column_text(1),
            .model = stmt.column_text(2),
            .scope_key = stmt.column_text(3),
            .agent_key = stmt.column_text(4),
            .origin_kind = stmt.column_text(5),
            .origin_ref = stmt.column_text(6),
            .message_count = stmt.column_int(7),
        });
    }
    return sessions;
}

void SessionStore::remove(const std::string &session_id) {
    std::scoped_lock lock(mutex_);
    sqlite::Transaction tx(db_);

    sqlite::Statement delete_bindings(db_, "DELETE FROM channel_session_bindings WHERE session_id = ?");
    delete_bindings.bind_text(1, session_id);
    static_cast<void>(delete_bindings.step());

    sqlite::Statement delete_messages(db_, "DELETE FROM messages WHERE session_id = ?");
    delete_messages.bind_text(1, session_id);
    static_cast<void>(delete_messages.step());

    sqlite::Statement delete_session(db_, "DELETE FROM sessions WHERE id = ?");
    delete_session.bind_text(1, session_id);
    static_cast<void>(delete_session.step());

    tx.commit();
}

std::optional<std::string> SessionStore::latest_session_id() {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT id FROM sessions ORDER BY created_at DESC, rowid DESC LIMIT 1");
    if (stmt.step()) {
        return stmt.column_text(0);
    }
    return std::nullopt;
}

void SessionStore::bind_jid(const std::string &jid, const std::string &session_id, const std::string &agent_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                                "VALUES (?, ?, ?, datetime('now')) "
                                "ON CONFLICT(jid, agent_key) DO UPDATE SET session_id = excluded.session_id, updated_at = datetime('now')");
    stmt.bind_text(1, jid);
    stmt.bind_text(2, agent_key);
    stmt.bind_text(3, session_id);
    static_cast<void>(stmt.step());
}

void SessionStore::clear_jid(const std::string &jid, const std::string &agent_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "DELETE FROM channel_session_bindings WHERE jid = ? AND agent_key = ?");
    stmt.bind_text(1, jid);
    stmt.bind_text(2, agent_key);
    static_cast<void>(stmt.step());
}

std::optional<std::string> SessionStore::bound_session_for_jid(const std::string &jid, const std::string &agent_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT session_id FROM channel_session_bindings WHERE jid = ? AND agent_key = ?");
    stmt.bind_text(1, jid);
    stmt.bind_text(2, agent_key);
    if (stmt.step()) {
        return stmt.column_text(0);
    }
    return std::nullopt;
}

bool SessionStore::session_belongs_to_scope(const std::string &session_id, const std::string &scope_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT 1 FROM sessions WHERE id = ? AND scope_key = ? LIMIT 1");
    stmt.bind_text(1, session_id);
    stmt.bind_text(2, scope_key);
    return stmt.step();
}

bool SessionStore::session_belongs_to_agent(const std::string &session_id, const std::string &agent_key) {
    std::scoped_lock lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT 1 FROM sessions WHERE id = ? AND agent_key = ? LIMIT 1");
    stmt.bind_text(1, session_id);
    stmt.bind_text(2, agent_key);
    return stmt.step();
}

SessionMetadata SessionStore::make_legacy_metadata(const std::string &model, const std::string &scope_key) {
    return SessionMetadata{
        .model = model,
        .scope_key = scope_key,
        .agent_key = "",
        .origin_kind = "cli",
        .origin_ref = "",
    };
}

} // namespace orangutan
