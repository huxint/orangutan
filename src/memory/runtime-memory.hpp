#pragma once

#include "types/types.hpp"
#include "memory/memory-mirror.hpp"
#include "memory/memory-store.hpp"
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

        void remember(const std::string &key, const std::string &content, const std::string &category = "general", const std::string &source = "manual",
                      base::f64 importance = 0.5);
        void update(const std::string &key, const std::string &content, const std::string &category = {}, bool merge = true, const std::string &source = {},
                    base::f64 importance = 0.5);

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
        std::size_t auto_capture(const std::string &text, const std::string &source = "auto:user");

        [[nodiscard]]
        MemoryMirrorRefreshResult refresh_mirror() const;

        [[nodiscard]]
        JournalStoreResult store_journal_summary(const std::string &summary, const std::string &source = "session:journal");

        [[nodiscard]]
        static bool query_requests_journal(std::string_view query);

    private:
        MemoryStore &store_;
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
