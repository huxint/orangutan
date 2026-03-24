#include "features/memory/memory-schema.hpp"

#include <cstdlib>
#include <filesystem>

namespace orangutan::memory_detail {

namespace {

constexpr char storage_delimiter = '\x1f';

} // namespace

std::filesystem::path default_db_path() {
    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        return std::filesystem::path{"orangutan_memory.db"};
    }

    auto dir = std::filesystem::path(home) / ".orangutan";
    std::filesystem::create_directories(dir);
    return dir / "memory.db";
}

MemorySchema inspect_memory_schema(sqlite::Database &db) {
    sqlite::Statement stmt(db, "PRAGMA table_info(memories)");
    MemorySchema schema;
    while (stmt.step()) {
        const auto column_name = stmt.column_text(1);
        schema.has_scope = schema.has_scope || column_name == "scope";
        schema.has_memory_key = schema.has_memory_key || column_name == "memory_key";
        schema.has_source = schema.has_source || column_name == "source";
        schema.has_importance = schema.has_importance || column_name == "importance";
        schema.has_access_count = schema.has_access_count || column_name == "access_count";
        schema.has_last_accessed_at = schema.has_last_accessed_at || column_name == "last_accessed_at";
    }
    return schema;
}

void create_current_schema(sqlite::Database &db) {
    db.exec(
        R"(
        CREATE TABLE IF NOT EXISTS memories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scope TEXT NOT NULL DEFAULT '',
            memory_key TEXT NOT NULL,
            content TEXT NOT NULL,
            category TEXT NOT NULL DEFAULT 'general',
            source TEXT NOT NULL DEFAULT 'manual',
            importance REAL NOT NULL DEFAULT 0.5,
            access_count INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now')),
            last_accessed_at TEXT,
            UNIQUE(scope, memory_key)
        );
        CREATE INDEX IF NOT EXISTS idx_memories_scope_updated ON memories(scope, updated_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_memories_scope_category ON memories(scope, category, updated_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_memories_scope_access ON memories(scope, access_count DESC, updated_at DESC, id DESC);
        )",
        "Failed to create memory schema");
}

bool enable_fts_if_available(sqlite::Database &db) {
    if (!db.try_exec("CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(memory_key, content, category, scope UNINDEXED, tokenize='unicode61');")) {
        return false;
    }

    if (!db.try_exec("CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
                     "INSERT INTO memories_fts(rowid, memory_key, content, category, scope) VALUES (new.id, new.memory_key, new.content, new.category, new.scope); "
                     "END;")) {
        return false;
    }

    if (!db.try_exec("CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
                     "DELETE FROM memories_fts WHERE rowid = old.id; "
                     "END;")) {
        return false;
    }

    if (!db.try_exec("CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
                     "DELETE FROM memories_fts WHERE rowid = old.id; "
                     "INSERT INTO memories_fts(rowid, memory_key, content, category, scope) VALUES (new.id, new.memory_key, new.content, new.category, new.scope); "
                     "END;")) {
        return false;
    }

    if (!db.try_exec("DELETE FROM memories_fts;")) {
        return false;
    }
    if (!db.try_exec("INSERT INTO memories_fts(rowid, memory_key, content, category, scope) "
                     "SELECT id, memory_key, content, category, scope FROM memories;")) {
        return false;
    }

    return true;
}

std::pair<std::string, std::string> parse_legacy_storage_key(std::string_view stored_key) {
    const auto pos = stored_key.find(storage_delimiter);
    if (pos == std::string_view::npos) {
        return {{}, std::string(stored_key)};
    }

    const auto scope_tag = stored_key.substr(0, pos);
    const auto key = stored_key.substr(pos + 1);
    if (scope_tag == "g") {
        return {{}, std::string(key)};
    }
    if (scope_tag.starts_with("s:")) {
        return {std::string(scope_tag.substr(2)), std::string(key)};
    }
    return {std::string(scope_tag), std::string(key)};
}

void migrate_legacy_memories(sqlite::Database &db) {
    db.exec("ALTER TABLE memories RENAME TO memories_legacy;", "Failed to rename legacy memories table");
    create_current_schema(db);

    sqlite::Statement read_legacy(db, "SELECT id, key, content, category, created_at, updated_at FROM memories_legacy ORDER BY id ASC");
    sqlite::Statement insert_new(db, "INSERT INTO memories (id, scope, memory_key, content, category, source, importance, access_count, created_at, updated_at, last_accessed_at) "
                                     "VALUES (?, ?, ?, ?, ?, 'manual', 0.5, 0, ?, ?, NULL)");

    while (read_legacy.step()) {
        const auto id = read_legacy.column_int(0);
        const auto stored_key = read_legacy.column_text(1);
        const auto content = read_legacy.column_text(2);
        const auto category = read_legacy.column_text(3);
        const auto created_at = read_legacy.column_text(4);
        const auto updated_at = read_legacy.column_text(5);
        const auto [scope, memory_key] = parse_legacy_storage_key(stored_key);

        insert_new.bind_int(1, id);
        insert_new.bind_text(2, scope);
        insert_new.bind_text(3, memory_key);
        insert_new.bind_text(4, content);
        insert_new.bind_text(5, category.empty() ? std::string{"general"} : category);
        insert_new.bind_text(6, created_at);
        insert_new.bind_text(7, updated_at);
        static_cast<void>(insert_new.step());
        insert_new.reset();
    }

    db.exec("DROP TABLE memories_legacy;", "Failed to drop legacy memories table");
}

} // namespace orangutan::memory_detail
