#pragma once

#include "core/types.hpp"
#include "infra/storage/sqlite.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace orangutan {

    struct MemoryRecord {
        int id = 0;
        std::string key;
        std::string content;
        std::string category;
        std::string scope;
        std::string source;
        std::string updated_at;
        base::f64 importance = 0.5;
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

        void remember(const std::string &key, const std::string &content, const std::string &category = "general", const std::string &scope = {},
                      const std::string &source = "manual", base::f64 importance = 0.5);

        void update(const std::string &key, const std::string &content, const std::string &category = {}, const std::string &scope = {}, bool merge = true,
                    const std::string &source = {}, base::f64 importance = 0.5);

        [[nodiscard]]
        std::vector<MemoryRecord> search(const std::string &query, const std::string &scope = {}, size_t limit = 8);

        [[nodiscard]]
        std::string recall(const std::string &query, const std::string &scope = {}, size_t limit = 8);

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> recall_by_category(const std::string &category, const std::string &scope = {}, size_t limit = 20);

        [[nodiscard]]
        std::vector<MemoryRecord> list(const std::string &scope = {}, const std::string &category = {}, size_t limit = 20);

        [[nodiscard]]
        MemoryStats stats(const std::string &scope = {});

        [[nodiscard]]
        bool forget(const std::string &key, const std::string &scope = {});

        [[nodiscard]]
        std::string dump_all(const std::string &scope = {}, size_t limit = 50);

        [[nodiscard]]
        size_t auto_capture(const std::string &text, const std::string &scope = {}, const std::string &source = "auto:user");

    private:
        sqlite::Database db_;
        mutable std::mutex mutex_;
        bool fts_enabled_ = false;

        void ensure_schema();
    };

} // namespace orangutan
