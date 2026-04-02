#pragma once

#include "memory/memory-store.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace orangutan::memory::detail {

    struct MemorySchema {
        bool has_scope = false;
        bool has_memory_key = false;
        bool has_source = false;
        bool has_importance = false;
        bool has_access_count = false;
        bool has_last_accessed_at = false;
        bool has_type = false;
    };

    [[nodiscard]]
    std::filesystem::path default_db_path();
    [[nodiscard]]
    MemorySchema inspect_memory_schema(sqlite::Database &db);
    void create_current_schema(sqlite::Database &db);
    [[nodiscard]]
    bool enable_fts_if_available(sqlite::Database &db);
    [[nodiscard]]
    std::pair<std::string, std::string> parse_legacy_storage_key(std::string_view stored_key);
    void migrate_legacy_memories(sqlite::Database &db);
    void add_type_column(sqlite::Database &db);

} // namespace orangutan::memory::detail
