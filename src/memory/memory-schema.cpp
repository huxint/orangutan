#include "memory/memory-schema.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace orangutan::memory::detail {
    namespace {

        constexpr int MEMORY_SCHEMA_VERSION = 1;

        [[nodiscard]]
        sqlite::SqliteResult<int> current_schema_version(sqlite::Database &db) {
            auto query = db.query("PRAGMA user_version");
            if (!query) {
                return std::unexpected(query.error());
            }
            return query->one<int>();
        }

        void throw_if_failed(sqlite::SqliteResult<void> result) {
            if (!result) {
                throw std::runtime_error(result.error().to_string());
            }
        }

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

    void create_current_schema(sqlite::Database &db) {
        const auto version = current_schema_version(db);
        if (!version) {
            throw std::runtime_error(version.error().to_string());
        }
        if (*version == MEMORY_SCHEMA_VERSION) {
            return;
        }

        throw_if_failed(db.exec_script(
            R"(
        DROP TRIGGER IF EXISTS memories_ai;
        DROP TRIGGER IF EXISTS memories_ad;
        DROP TRIGGER IF EXISTS memories_au;
        DROP TABLE IF EXISTS memories_fts;
        DROP TABLE IF EXISTS memory_meta;
        DROP TABLE IF EXISTS memories;

        CREATE TABLE memories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scope TEXT NOT NULL,
            memory_key TEXT NOT NULL,
            content TEXT NOT NULL,
            kind TEXT NOT NULL DEFAULT 'user',
            updated_at TEXT NOT NULL DEFAULT (datetime('now')),
            UNIQUE(scope, memory_key)
        );
        CREATE INDEX idx_memories_scope_updated ON memories(scope, updated_at DESC, id DESC);
        PRAGMA user_version = 1;
        )",
            "Failed to create memory schema"));
    }

} // namespace orangutan::memory::detail
