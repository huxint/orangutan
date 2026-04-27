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
        memory_type kind = memory_type::user;
        std::string scope;
        std::string updated_at;
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

        void remember(std::string_view key, std::string_view content, memory_type kind = memory_type::user, std::string_view scope = {});

        [[nodiscard]]
        std::vector<MemoryRecord> search(std::string_view query, std::string_view scope = {}, std::size_t limit = 8);

        [[nodiscard]]
        std::string recall(std::string_view query, std::string_view scope = {}, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<MemoryRecord> list(std::string_view scope = {}, std::size_t limit = 20);

        [[nodiscard]]
        bool forget(std::string_view key, std::string_view scope = {});

    private:
        sqlite::Database db_;
        mutable std::mutex mutex_;

        void ensure_schema();
    };

} // namespace orangutan::memory

namespace orangutan {

    namespace memory::detail {}
    namespace memory_detail = memory::detail;

    using memory::MemoryRecord;
    using memory::MemoryStore;

} // namespace orangutan
