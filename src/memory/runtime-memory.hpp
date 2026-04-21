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

        void remember(std::string_view key, std::string_view content, std::string_view category = "general", memory_type type = memory_type::user,
                      std::string_view source = "manual", double importance = 0.5);
        void update(std::string_view key, std::string_view content, std::string_view category = {}, memory_type type = memory_type::user, bool merge = true,
                    std::string_view source = {}, double importance = 0.5);

        [[nodiscard]]
        bool forget(std::string_view key);

        [[nodiscard]]
        std::vector<MemoryRecord> search(std::string_view query, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<MemoryRecord> prompt_memories(std::string_view query, std::size_t limit = 8);

        [[nodiscard]]
        std::string recall(std::string_view query, std::size_t limit = 8);

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> recall_by_category(std::string_view category, std::size_t limit = 20);

        [[nodiscard]]
        std::vector<MemoryRecord> list(std::string_view category = {}, std::size_t limit = 20);

        [[nodiscard]]
        MemoryStats stats();

        [[nodiscard]]
        MemoryMirrorRefreshResult refresh_mirror() const;

        [[nodiscard]]
        JournalStoreResult store_journal_summary(std::string_view summary, std::string_view source = "session:journal");

        [[nodiscard]]
        static bool query_requests_journal(std::string_view query);

        /// Consolidate memories: prune stale/low-importance, enforce limits.
        [[nodiscard]]
        std::size_t consolidate(std::size_t max_per_scope = 200, int stale_days = 90, double stale_importance_threshold = 0.3);

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
