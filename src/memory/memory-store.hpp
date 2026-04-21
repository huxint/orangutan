#pragma once

#include "types/types.hpp"
#include "memory/memory-type.hpp"
#include "storage/sqlite.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::memory {

    struct MemoryRecord {
        int id = 0;
        std::string key;
        std::string content;
        std::string category;
        memory_type type = memory_type::user;
        std::string scope;
        std::string source;
        std::string updated_at;
        double importance = 0.5;
        int access_count = 0;
    };

    struct MemoryStats {
        int total = 0;
        int categories = 0;
        int manual_entries = 0;
        int auto_entries = 0;
        int journal_entries = 0;
    };

    class MemoryStore {
    public:
        MemoryStore();
        explicit MemoryStore(const std::filesystem::path &db_path);
        ~MemoryStore();

        MemoryStore(const MemoryStore &) = delete;
        MemoryStore &operator=(const MemoryStore &) = delete;
        MemoryStore(MemoryStore &&) = delete;
        MemoryStore &operator=(MemoryStore &&) = delete;

        void remember(std::string_view key, std::string_view content, std::string_view category = "general", memory_type type = memory_type::user, std::string_view scope = {},
                      std::string_view source = "manual", double importance = 0.5);

        void update(std::string_view key, std::string_view content, std::string_view category = {}, memory_type type = memory_type::user, std::string_view scope = {},
                    bool merge = true, std::string_view source = {}, double importance = 0.5);

        [[nodiscard]]
        std::vector<MemoryRecord> search(std::string_view query, std::string_view scope = {}, std::size_t limit = 8);

        [[nodiscard]]
        std::string recall(std::string_view query, std::string_view scope = {}, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> recall_by_category(std::string_view category, std::string_view scope = {}, std::size_t limit = 20);

        [[nodiscard]]
        std::vector<MemoryRecord> list(std::string_view scope = {}, std::string_view category = {}, std::size_t limit = 20);

        [[nodiscard]]
        MemoryStats stats(std::string_view scope = {});

        [[nodiscard]]
        bool forget(std::string_view key, std::string_view scope = {});

        [[nodiscard]]
        std::string dump_all(std::string_view scope = {}, std::size_t limit = 50);

        /// Consolidate memories: prune stale low-importance entries, enforce per-scope limits.
        /// Returns the number of records pruned.
        [[nodiscard]]
        std::size_t consolidate(std::string_view scope = {}, std::size_t max_per_scope = 200, int stale_days = 90, double stale_importance_threshold = 0.3);

        /// Generate a concise manifest listing of all non-journal memories.
        [[nodiscard]]
        std::string manifest(std::string_view scope = {}, std::size_t limit = 200);

    private:
        sqlite::Database db_;
        mutable std::mutex mutex_;
        bool fts_enabled_ = false;

        void ensure_schema();
    };

} // namespace orangutan::memory

namespace orangutan {

    namespace memory::detail {}
    namespace memory_detail = memory::detail;

    using memory::MemoryRecord;
    using memory::MemoryStats;
    using memory::MemoryStore;

} // namespace orangutan
