#include "storage/session-store.hpp"
#include "storage/sqlite-throwing.hpp"
#include "types/base.hpp"
#include "utils/format.hpp"
#include "utils/overloaded.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <spdlog/spdlog.h>

namespace orangutan::storage {

    namespace {

        std::filesystem::path db_path() {
            const char *home = std::getenv("HOME");
            if (home == nullptr) {
                return std::filesystem::path{"orangutan_sessions.db"};
            }

            return std::filesystem::path(home) / ".orangutan" / "sessions.db";
        }

        nlohmann::json serialize_content(const Message &message) {
            auto arr = nlohmann::json::array();
            for (const auto &block : message) {
                arr.push_back(content_block_to_json(block));
            }
            return arr;
        }

        std::vector<Content> deserialize_content(std::string_view json_str) {
            std::vector<Content> blocks;
            const auto arr = nlohmann::json::parse(json_str);

            for (const auto &item : arr) {
                const auto type = item.at("type").get<std::string>();
                if (type == "text") {
                    blocks.emplace_back(Text{item.at("text").get<std::string>()});
                    continue;
                }
                if (type == "tool_use") {
                    blocks.emplace_back(ToolUse(item.at("id").get<std::string>(), item.at("name").get<std::string>(), item.at("input")));
                    continue;
                }
                if (type == "tool_result") {
                    const auto &content_field = item.at("content");
                    ToolResult result;
                    result.tool_use_id = item.at("tool_use_id").get<std::string>();
                    result.is_error = item.value("is_error", false);
                    if (content_field.is_string()) {
                        result.content = content_field.get<std::string>();
                    } else if (content_field.is_array()) {
                        for (const auto &part : content_field) {
                            const auto part_type = part.value("type", std::string{});
                            if (part_type == "text") {
                                result.content += part.value("text", std::string{});
                            } else if (part_type == "image") {
                                if (part.contains("source")) {
                                    result.images.push_back({
                                        .media_type = part["source"].value("media_type", std::string{}),
                                        .data = part["source"].value("data", std::string{}),
                                    });
                                }
                            }
                        }
                    }
                    blocks.emplace_back(std::move(result));
                    continue;
                }
                if (type == "thinking") {
                    blocks.emplace_back(Thinking{item.at("thinking").get<std::string>()});
                }
            }

            return blocks;
        }

        using session_info_row = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string, int>;

        auto make_session_info(const session_info_row &row) -> SessionInfo {
            return SessionInfo{
                .id = std::get<0>(row),
                .created_at = std::get<1>(row),
                .model = std::get<2>(row),
                .scope_key = std::get<3>(row),
                .agent_key = std::get<4>(row),
                .origin_kind = std::get<5>(row),
                .origin_ref = std::get<6>(row),
                .message_count = std::get<7>(row),
            };
        }

        void ensure_column(sqlite::Database &db, std::string_view table_name, std::string_view column_name, std::string_view add_sql) {
            auto pragma = sqlite::prepare_or_throw(db, std::string("PRAGMA table_info(") + std::string(table_name) + ")");
            while (sqlite::unwrap(pragma.step())) {
                auto row = sqlite::unwrap(pragma.row());
                if (sqlite::unwrap(row.get<std::string>(1)) == column_name) {
                    return;
                }
            }

            sqlite::exec_script(db, add_sql, "Failed to migrate session schema");
        }

        struct ChannelBindingSchema {
            bool has_agent_key = false;
            int jid_pk_position = 0;
            int agent_key_pk_position = 0;
        };

        ChannelBindingSchema inspect_channel_binding_schema(sqlite::Database &db) {
            auto pragma = sqlite::prepare_or_throw(db, "PRAGMA table_info(channel_session_bindings)");

            ChannelBindingSchema schema;
            while (sqlite::unwrap(pragma.step())) {
                auto row = sqlite::unwrap(pragma.row());
                const auto column_name = sqlite::unwrap(row.get<std::string>(1));
                const auto pk_position = sqlite::unwrap(row.get<int>(5));

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
            sqlite::exec_script(db, "DROP INDEX IF EXISTS idx_channel_session_bindings_updated;", "Failed to drop old binding index");
            sqlite::exec_script(db, "ALTER TABLE channel_session_bindings RENAME TO channel_session_bindings_legacy;", "Failed to rename legacy binding table");
            sqlite::exec_script(db,
                                "CREATE TABLE channel_session_bindings ("
                                "jid TEXT NOT NULL,"
                                "agent_key TEXT NOT NULL DEFAULT '',"
                                "session_id TEXT NOT NULL,"
                                "updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
                                "PRIMARY KEY (jid, agent_key),"
                                "FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE"
                                ");",
                                "Failed to create upgraded binding table");

            sqlite::exec_script(db,
                                legacy_has_agent_key ? "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                                                       "SELECT jid, COALESCE(agent_key, ''), session_id, updated_at FROM channel_session_bindings_legacy;"
                                                     : "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                                                       "SELECT jid, '', session_id, updated_at FROM channel_session_bindings_legacy;",
                                "Failed to migrate legacy channel bindings");
            sqlite::exec_script(db, "DROP TABLE channel_session_bindings_legacy;", "Failed to drop legacy binding table");
            sqlite::exec_script(db, "CREATE INDEX IF NOT EXISTS idx_channel_session_bindings_updated ON channel_session_bindings(updated_at);",
                                "Failed to recreate binding index");
        }

        void write_messages(sqlite::Database &db, std::string_view session_id, const std::vector<Message> &messages, std::size_t start_index) {
            auto insert_msg = sqlite::prepare_or_throw(db, "INSERT INTO messages (session_id, seq, role, content_json) VALUES (?, ?, ?, ?)");
            for (std::size_t index = start_index; index < messages.size(); ++index) {
                const auto &message = messages[index];
                const auto content_json = serialize_content(message).dump();

                sqlite::unwrap(insert_msg.clear_bindings());
                insert_msg.bind_all(session_id, static_cast<int>(index), magic_enum::enum_name(message.role()), content_json);
                sqlite::unwrap(insert_msg.step());
                sqlite::unwrap(insert_msg.reset());
            }
        }

        void insert_session(sqlite::Database &db, std::string_view session_id, const SessionMetadata &metadata) {
            sqlite::exec_bind(db,
                              "INSERT INTO sessions (id, model, scope_key, agent_key, origin_kind, origin_ref) VALUES (?, ?, ?, ?, ?, ?)",
                              session_id, metadata.model, metadata.scope_key, metadata.agent_key, metadata.origin_kind, metadata.origin_ref);
        }

        void update_session_model(sqlite::Database &db, std::string_view session_id, std::string_view model) {
            if (model.empty()) {
                return;
            }

            sqlite::exec_bind(db, "UPDATE sessions SET model = ? WHERE id = ?", model, session_id);
        }

        void update_session_metadata(sqlite::Database &db, std::string_view session_id, const SessionMetadata &metadata) {
            sqlite::exec_bind(db,
                              "UPDATE sessions "
                              "SET model = ?, scope_key = ?, agent_key = ?, origin_kind = ?, origin_ref = ? "
                              "WHERE id = ?",
                              metadata.model, metadata.scope_key, metadata.agent_key, metadata.origin_kind, metadata.origin_ref, session_id);
        }

        bool session_exists(sqlite::Database &db, std::string_view session_id) {
            return sqlite::query_optional<int>(db, "SELECT 1 FROM sessions WHERE id = ? LIMIT 1", session_id).has_value();
        }

        void append_permission_rule(ToolPermissionContext &ctx, PermissionRule rule) {
            switch (rule.behavior) {
                case permission_behavior::allow:
                    ctx.allow_rules.push_back(std::move(rule));
                    break;
                case permission_behavior::deny:
                    ctx.deny_rules.push_back(std::move(rule));
                    break;
                case permission_behavior::ask:
                    ctx.ask_rules.push_back(std::move(rule));
                    break;
            }
        }

        void remove_session_rules(std::vector<PermissionRule> &rules) {
            rules.erase(std::remove_if(rules.begin(), rules.end(),
                                       [](const PermissionRule &rule) {
                                           return rule.source == permission_rule_source::session;
                                       }),
                        rules.end());
        }

        void append_session_rules_from_context(std::vector<PermissionRule> &target, const std::vector<PermissionRule> &rules) {
            for (const auto &rule : rules) {
                if (rule.source == permission_rule_source::session) {
                    target.push_back(rule);
                }
            }
        }

        void bind_session_permission_rule(sqlite::Statement &stmt, std::string_view session_id, const PermissionRule &rule) {
            stmt.bind_all(session_id,
                          static_cast<int>(rule.behavior),
                          rule.tool_name,
                          rule.content.has_value() ? 1 : 0,
                          rule.content.has_value() ? static_cast<int>(rule.content->match_type) : static_cast<int>(rule_match_type::exact),
                          rule.content.has_value() ? std::string_view{rule.content->pattern} : std::string_view{});
        }

    } // namespace

    SessionStore::SessionStore()
    : SessionStore(db_path()) {}

    SessionStore::SessionStore(const std::filesystem::path &path)
    : db_(sqlite::open_or_throw(path)) {
        sqlite::exec_script(db_, "PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
        ensure_schema();
    }

    SessionStore::~SessionStore() = default;

    void SessionStore::ensure_schema() {
        sqlite::exec_script(db_, R"(
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
        CREATE TABLE IF NOT EXISTS session_permission_rules (
            session_id TEXT NOT NULL,
            behavior INTEGER NOT NULL,
            tool_name TEXT NOT NULL,
            has_content INTEGER NOT NULL DEFAULT 0,
            content_match_type INTEGER NOT NULL DEFAULT 0,
            content_pattern TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (session_id, behavior, tool_name, has_content, content_match_type, content_pattern),
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq);
        CREATE INDEX IF NOT EXISTS idx_channel_session_bindings_updated ON channel_session_bindings(updated_at);
        CREATE INDEX IF NOT EXISTS idx_session_permission_rules_session ON session_permission_rules(session_id, created_at);
    )",
                            "Failed to create session schema");

        ensure_column(db_, "sessions", "scope_key", "ALTER TABLE sessions ADD COLUMN scope_key TEXT NOT NULL DEFAULT ''");
        ensure_column(db_, "sessions", "agent_key", "ALTER TABLE sessions ADD COLUMN agent_key TEXT NOT NULL DEFAULT ''");
        ensure_column(db_, "sessions", "origin_kind", "ALTER TABLE sessions ADD COLUMN origin_kind TEXT NOT NULL DEFAULT 'cli'");
        ensure_column(db_, "sessions", "origin_ref", "ALTER TABLE sessions ADD COLUMN origin_ref TEXT NOT NULL DEFAULT ''");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_sessions_scope_key ON sessions(scope_key, created_at DESC);", "Failed to create session scope index");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_sessions_agent_key ON sessions(agent_key, created_at DESC);", "Failed to create session agent index");

        const auto binding_schema = inspect_channel_binding_schema(db_);
        if (!binding_schema.has_agent_key || binding_schema.jid_pk_position != 1 || binding_schema.agent_key_pk_position != 2) {
            sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
                rebuild_channel_binding_table(tx, binding_schema.has_agent_key);
            }));
        }
    }

    std::string SessionStore::generate_uuid() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<base::u32> dist;

        return utils::format("{:x}-{:x}-{:x}-{:x}-{:x}{:x}", dist(gen), dist(gen) & 0xFFFF, 0x4000 | (dist(gen) & 0x0FFF), 0x8000 | (dist(gen) & 0x3FFF), dist(gen),
                                       dist(gen) & 0xFFFF);
    }

    std::string SessionStore::save(const std::vector<Message> &messages, const SessionMetadata &metadata) {
        std::scoped_lock lock(mutex_);
        const auto session_id = generate_uuid();
        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            insert_session(tx, session_id, metadata);
            write_messages(tx, session_id, messages, 0);
        }));
        spdlog::info("Saved session {} ({} messages)", session_id, messages.size());
        return session_id;
    }

    std::string SessionStore::create_empty(const SessionMetadata &metadata) {
        std::scoped_lock lock(mutex_);
        const auto session_id = generate_uuid();
        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            insert_session(tx, session_id, metadata);
        }));
        spdlog::info("Created empty session {}", session_id);
        return session_id;
    }

    void SessionStore::update(std::string_view session_id, const std::vector<Message> &messages, std::string_view model) {
        std::scoped_lock lock(mutex_);
        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            update_session_model(tx, session_id, model);
            sqlite::exec_bind(tx, "DELETE FROM messages WHERE session_id = ?", session_id);
            write_messages(tx, session_id, messages, 0);
        }));
        spdlog::info("Updated session {} ({} messages)", session_id, messages.size());
    }

    void SessionStore::update(std::string_view session_id, const std::vector<Message> &messages, const SessionMetadata &metadata) {
        std::scoped_lock lock(mutex_);
        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            update_session_metadata(tx, session_id, metadata);
            sqlite::exec_bind(tx, "DELETE FROM messages WHERE session_id = ?", session_id);
            write_messages(tx, session_id, messages, 0);
        }));
        spdlog::info("Updated session {} ({} messages)", session_id, messages.size());
    }

    void SessionStore::append(std::string_view session_id, const std::vector<Message> &messages, std::size_t start_index, std::string_view model) {
        std::scoped_lock lock(mutex_);
        if (start_index > messages.size()) {
            throw std::runtime_error("append start_index is out of range");
        }

        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            update_session_model(tx, session_id, model);
            write_messages(tx, session_id, messages, start_index);
        }));

        spdlog::info("Appended {} message(s) to session {}", messages.size() - start_index, session_id);
    }

    void SessionStore::append(std::string_view session_id, const std::vector<Message> &messages, std::size_t start_index, const SessionMetadata &metadata) {
        std::scoped_lock lock(mutex_);
        if (start_index > messages.size()) {
            throw std::runtime_error("append start_index is out of range");
        }

        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            update_session_metadata(tx, session_id, metadata);
            write_messages(tx, session_id, messages, start_index);
        }));

        spdlog::info("Appended {} message(s) to session {}", messages.size() - start_index, session_id);
    }

    std::vector<Message> SessionStore::load(std::string_view session_id) {
        std::scoped_lock lock(mutex_);

        std::vector<Message> messages;
        const auto rows = sqlite::query_all<std::tuple<std::string, std::string>>(
            db_, "SELECT role, content_json FROM messages WHERE session_id = ? ORDER BY seq", session_id);
        for (const auto &[role_text, content_json] : rows) {
            Message message{magic_enum::enum_cast<base::role>(role_text).value_or(base::role::user)};
            for (auto &block : deserialize_content(content_json)) {
                std::visit(
                    utils::Overloaded{
                        [&](Text &item) { message.text(std::move(item)); },
                        [&](Thinking &item) { message.thinking(std::move(item)); },
                        [&](ToolUse &item) { message.tool_use(std::move(item)); },
                        [&](ToolResult &item) { message.tool_result(std::move(item)); },
                    },
                    block);
            }
            messages.push_back(std::move(message));
        }

        if (messages.empty() && !session_exists(db_, session_id)) {
            throw std::runtime_error("Session not found: " + std::string(session_id));
        }

        return messages;
    }

    void SessionStore::save_session_permission_rule(std::string_view session_id, PermissionRule rule) {
        if (session_id.empty()) {
            throw std::runtime_error("Session id is required to persist a session permission rule");
        }

        std::scoped_lock lock(mutex_);
        if (!session_exists(db_, session_id)) {
            throw std::runtime_error("Session not found: " + std::string(session_id));
        }

        rule.source = permission_rule_source::session;

        sqlite::exec_bind(db_,
                          "INSERT OR IGNORE INTO session_permission_rules "
                          "(session_id, behavior, tool_name, has_content, content_match_type, content_pattern) "
                          "VALUES (?, ?, ?, ?, ?, ?)",
                          session_id,
                          static_cast<int>(rule.behavior),
                          rule.tool_name,
                          rule.content.has_value() ? 1 : 0,
                          rule.content.has_value() ? static_cast<int>(rule.content->match_type) : static_cast<int>(rule_match_type::exact),
                          rule.content.has_value() ? std::string_view{rule.content->pattern} : std::string_view{});
    }

    std::vector<PermissionRule> SessionStore::load_session_permission_rules(std::string_view session_id) {
        if (session_id.empty()) {
            return {};
        }

        std::scoped_lock lock(mutex_);
        std::vector<PermissionRule> rules;
        const auto rows = sqlite::query_all<std::tuple<int, std::string, int, int, std::string>>(
            db_,
            "SELECT behavior, tool_name, has_content, content_match_type, content_pattern "
            "FROM session_permission_rules "
            "WHERE session_id = ? "
            "ORDER BY created_at ASC, rowid ASC",
            session_id);
        for (const auto &[behavior_value, tool_name, has_content_value, match_type_value, content_pattern] : rows) {
            const auto behavior = static_cast<permission_behavior>(behavior_value);
            const auto has_content = has_content_value != 0;

            PermissionRule rule{
                .source = permission_rule_source::session,
                .behavior = behavior,
                .tool_name = tool_name,
                .content = std::nullopt,
            };
            if (has_content) {
                rule.content = RuleContent{
                    .match_type = static_cast<rule_match_type>(match_type_value),
                    .pattern = content_pattern,
                };
            }
            rules.push_back(std::move(rule));
        }

        return rules;
    }

    ToolPermissionContext SessionStore::load_session_permission_context(std::string_view session_id, const ToolPermissionContext &base_context) {
        auto context = base_context;
        remove_session_rules(context.allow_rules);
        remove_session_rules(context.deny_rules);
        remove_session_rules(context.ask_rules);

        for (auto &rule : load_session_permission_rules(session_id)) {
            append_permission_rule(context, std::move(rule));
        }

        return context;
    }

    void SessionStore::clear_session_permission_rules(std::string_view session_id) {
        if (session_id.empty()) {
            return;
        }

        std::scoped_lock lock(mutex_);
        sqlite::exec_bind(db_, "DELETE FROM session_permission_rules WHERE session_id = ?", session_id);
    }

    void SessionStore::replace_session_permission_rules(std::string_view session_id, const ToolPermissionContext &context) {
        if (session_id.empty()) {
            return;
        }

        std::vector<PermissionRule> session_rules;
        append_session_rules_from_context(session_rules, context.allow_rules);
        append_session_rules_from_context(session_rules, context.deny_rules);
        append_session_rules_from_context(session_rules, context.ask_rules);

        std::scoped_lock lock(mutex_);
        if (!session_exists(db_, session_id)) {
            throw std::runtime_error("Session not found: " + std::string(session_id));
        }

        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            // Replace the persisted session-scoped subset atomically so resumed sessions mirror
            // the in-memory rules exactly, without leaking non-session rules into storage.
            sqlite::exec_bind(tx, "DELETE FROM session_permission_rules WHERE session_id = ?", session_id);

            auto insert = sqlite::prepare_or_throw(tx,
                                                   "INSERT OR IGNORE INTO session_permission_rules "
                                                   "(session_id, behavior, tool_name, has_content, content_match_type, content_pattern) "
                                                   "VALUES (?, ?, ?, ?, ?, ?)");
            for (auto rule : session_rules) {
                rule.source = permission_rule_source::session;
                sqlite::unwrap(insert.clear_bindings());
                bind_session_permission_rule(insert, session_id, rule);
                sqlite::unwrap(insert.step());
                sqlite::unwrap(insert.reset());
            }
        }));
    }

    std::vector<SessionInfo> SessionStore::list_sessions(std::string_view scope_key) {
        std::scoped_lock lock(mutex_);
        const auto rows = scope_key.empty()
                              ? sqlite::query_all<session_info_row>(
                                    db_,
                                    "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
                                    "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
                                    "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC")
                              : sqlite::query_all<session_info_row>(
                                    db_,
                                    "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
                                    "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
                                    "WHERE s.scope_key = ? "
                                    "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC",
                                    scope_key);

        std::vector<SessionInfo> sessions;
        sessions.reserve(rows.size());
        for (const auto &row : rows) {
            sessions.push_back(make_session_info(row));
        }
        return sessions;
    }

    std::vector<SessionInfo> SessionStore::list_sessions_for_agent(std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        const auto rows = sqlite::query_all<session_info_row>(
            db_,
            "SELECT s.id, s.created_at, s.model, s.scope_key, s.agent_key, s.origin_kind, s.origin_ref, COUNT(m.id) "
            "FROM sessions s LEFT JOIN messages m ON s.id = m.session_id "
            "WHERE s.agent_key = ? "
            "GROUP BY s.id ORDER BY s.created_at DESC, s.rowid DESC",
            agent_key);

        std::vector<SessionInfo> sessions;
        sessions.reserve(rows.size());
        for (const auto &row : rows) {
            sessions.push_back(make_session_info(row));
        }
        return sessions;
    }

    void SessionStore::remove(std::string_view session_id) {
        std::scoped_lock lock(mutex_);
        sqlite::unwrap(db_.transaction([&](sqlite::Database &tx) {
            sqlite::exec_bind(tx, "DELETE FROM channel_session_bindings WHERE session_id = ?", session_id);
            sqlite::exec_bind(tx, "DELETE FROM messages WHERE session_id = ?", session_id);
            sqlite::exec_bind(tx, "DELETE FROM sessions WHERE id = ?", session_id);
        }));
    }

    std::optional<std::string> SessionStore::latest_session_id() {
        std::scoped_lock lock(mutex_);
        return sqlite::query_optional<std::string>(db_, "SELECT id FROM sessions ORDER BY created_at DESC, rowid DESC LIMIT 1");
    }

    void SessionStore::bind_jid(std::string_view jid, std::string_view session_id, std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        sqlite::exec_bind(db_,
                          "INSERT INTO channel_session_bindings (jid, agent_key, session_id, updated_at) "
                          "VALUES (?, ?, ?, datetime('now')) "
                          "ON CONFLICT(jid, agent_key) DO UPDATE SET session_id = excluded.session_id, updated_at = datetime('now')",
                          jid, agent_key, session_id);
    }

    void SessionStore::clear_jid(std::string_view jid, std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        sqlite::exec_bind(db_, "DELETE FROM channel_session_bindings WHERE jid = ? AND agent_key = ?", jid, agent_key);
    }

    std::optional<std::string> SessionStore::bound_session_for_jid(std::string_view jid, std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        return sqlite::query_optional<std::string>(
            db_, "SELECT session_id FROM channel_session_bindings WHERE jid = ? AND agent_key = ?", jid, agent_key);
    }

    bool SessionStore::session_belongs_to_scope(std::string_view session_id, std::string_view scope_key) {
        std::scoped_lock lock(mutex_);
        return sqlite::query_optional<int>(
                   db_, "SELECT 1 FROM sessions WHERE id = ? AND scope_key = ? LIMIT 1", session_id, scope_key)
            .has_value();
    }

    bool SessionStore::session_belongs_to_agent(std::string_view session_id, std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        return sqlite::query_optional<int>(
                   db_, "SELECT 1 FROM sessions WHERE id = ? AND agent_key = ? LIMIT 1", session_id, agent_key)
            .has_value();
    }

} // namespace orangutan::storage
