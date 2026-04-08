#pragma once

#include "types/types.hpp"
#include "memory/memory-mirror.hpp"
#include "memory/memory-store.hpp"
#include "memory/memory-type.hpp"
#include "bootstrap/memory-context.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::memory {

    struct JournalStoreResult {
        bool stored = false;
        bool mirrored = false;
        std::string status;
        std::string key;
    };

    class RuntimeMemory {
    public:
        RuntimeMemory(MemoryStore &store, bootstrap::RuntimeMemoryContext context = {}, MemoryMirror mirror = {});

        [[nodiscard]]
        const bootstrap::RuntimeMemoryContext &context() const {
            return context_;
        }

        void remember(const std::string &key, const std::string &content, const std::string &category = "general", memory_type type = memory_type::user,
                      const std::string &source = "manual", base::f64 importance = 0.5);
        void update(const std::string &key, const std::string &content, const std::string &category = {}, memory_type type = memory_type::user, bool merge = true,
                    const std::string &source = {}, base::f64 importance = 0.5);

        [[nodiscard]]
        bool forget(const std::string &key);

        [[nodiscard]]
        std::vector<MemoryRecord> search(const std::string &query, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<MemoryRecord> prompt_memories(const std::string &query, std::size_t limit = 8);

        [[nodiscard]]
        std::string recall(const std::string &query, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> recall_by_category(const std::string &category, std::size_t limit = 20);

        [[nodiscard]]
        std::vector<MemoryRecord> list(const std::string &category = {}, std::size_t limit = 20);

        [[nodiscard]]
        MemoryStats stats();

        [[nodiscard]]
        MemoryMirrorRefreshResult refresh_mirror() const;

        [[nodiscard]]
        JournalStoreResult store_journal_summary(const std::string &summary, const std::string &source = "session:journal");

        [[nodiscard]]
        static bool query_requests_journal(std::string_view query);

        /// Consolidate memories: prune stale/low-importance, enforce limits.
        [[nodiscard]]
        std::size_t consolidate(std::size_t max_per_scope = 200, int stale_days = 90, base::f64 stale_importance_threshold = 0.3);

        /// Generate a concise manifest of all stored memories (excluding journals).
        [[nodiscard]]
        std::string manifest(std::size_t limit = 200);

    private:
        MemoryStore *store_ = nullptr;
        bootstrap::RuntimeMemoryContext context_;
        MemoryMirror mirror_;

        [[nodiscard]]
        std::vector<MemoryRecord> durable_records() const;

        void refresh_mirror_after_write() const;
    };

} // namespace orangutan::memory

namespace orangutan {

    using memory::JournalStoreResult;
    using memory::RuntimeMemory;

} // namespace orangutan
