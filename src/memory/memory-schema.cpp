#include "memory/memory-schema.hpp"
#include <cstdlib>
#include <filesystem>

namespace orangutan::memory::detail {

    std::filesystem::path default_db_path() {
        const char *home = std::getenv("HOME");
        if (home == nullptr) {
            return std::filesystem::path{"orangutan_memory.db"};
        }

        auto dir = std::filesystem::path(home) / ".orangutan";
        std::filesystem::create_directories(dir);
        return dir / "memory.db";
    }

    void create_current_schema(sqlite::Database &db) {
        db.exec_script(
            R"(
        CREATE TABLE IF NOT EXISTS memories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scope TEXT NOT NULL DEFAULT '',
            memory_key TEXT NOT NULL,
            content TEXT NOT NULL,
            category TEXT NOT NULL DEFAULT 'general',
            type TEXT NOT NULL DEFAULT 'user',
            source TEXT NOT NULL DEFAULT 'manual',
            importance REAL NOT NULL DEFAULT 0.5,
            access_count INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now')),
            last_accessed_at TEXT,
            UNIQUE(scope, memory_key)
        );
        )",
            "Failed to create memory schema");

        db.exec_script(
            R"(
        CREATE INDEX IF NOT EXISTS idx_memories_scope_updated ON memories(scope, updated_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_memories_scope_category ON memories(scope, category, updated_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_memories_scope_access ON memories(scope, access_count DESC, updated_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_memories_scope_type ON memories(scope, type, updated_at DESC, id DESC);
        )",
            "Failed to create memory indexes");
    }

    bool enable_fts_if_available(sqlite::Database &db) {
        if (!db.try_exec_script("CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(memory_key, content, category, type, scope UNINDEXED, tokenize='unicode61');")) {
            return false;
        }

        if (!db.try_exec_script(
                "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
                "INSERT INTO memories_fts(rowid, memory_key, content, category, type, scope) VALUES (new.id, new.memory_key, new.content, new.category, new.type, new.scope); "
                "END;")) {
            return false;
        }

        if (!db.try_exec_script("CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
                                "DELETE FROM memories_fts WHERE rowid = old.id; "
                                "END;")) {
            return false;
        }

        if (!db.try_exec_script(
                "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
                "DELETE FROM memories_fts WHERE rowid = old.id; "
                "INSERT INTO memories_fts(rowid, memory_key, content, category, type, scope) VALUES (new.id, new.memory_key, new.content, new.category, new.type, new.scope); "
                "END;")) {
            return false;
        }

        if (!db.try_exec_script("DELETE FROM memories_fts;")) {
            return false;
        }
        if (!db.try_exec_script("INSERT INTO memories_fts(rowid, memory_key, content, category, type, scope) "
                                "SELECT id, memory_key, content, category, type, scope FROM memories;")) {
            return false;
        }

        return true;
    }

} // namespace orangutan::memory::detail
