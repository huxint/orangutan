#pragma once

#include "features/memory/memory-mirror.hpp"
#include "features/memory/memory.hpp"
#include "app/runtime/memory-context.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan {

    struct JournalStoreResult {
        bool stored = false;
        bool mirrored = false;
        std::string status;
        std::string key;
    };

    class RuntimeMemory {
    public:
        RuntimeMemory(MemoryStore &store, RuntimeMemoryContext context = {}, MemoryMirror mirror = {});

        [[nodiscard]]
        const RuntimeMemoryContext &context() const {
            return context_;
        }

        void remember(const std::string &key, const std::string &content, const std::string &category = "general", const std::string &source = "manual", double importance = 0.5);
        void update(const std::string &key, const std::string &content, const std::string &category = {}, bool merge = true, const std::string &source = {},
                    double importance = 0.5);

        [[nodiscard]]
        bool forget(const std::string &key);

        [[nodiscard]]
        std::vector<MemoryRecord> search(const std::string &query, size_t limit = 8);

        [[nodiscard]]
        std::vector<MemoryRecord> prompt_memories(const std::string &query, size_t limit = 8);

        [[nodiscard]]
        std::string recall(const std::string &query, size_t limit = 8);

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> recall_by_category(const std::string &category, size_t limit = 20);

        [[nodiscard]]
        std::vector<MemoryRecord> list(const std::string &category = {}, size_t limit = 20);

        [[nodiscard]]
        MemoryStats stats();

        [[nodiscard]]
        size_t auto_capture(const std::string &text, const std::string &source = "auto:user");

        [[nodiscard]]
        MemoryMirrorRefreshResult refresh_mirror() const;

        [[nodiscard]]
        JournalStoreResult store_journal_summary(const std::string &summary, const std::string &source = "session:journal");

        [[nodiscard]]
        static bool query_requests_journal(std::string_view query);

    private:
        MemoryStore &store_;
        RuntimeMemoryContext context_;
        MemoryMirror mirror_;

        [[nodiscard]]
        std::vector<MemoryRecord> durable_records() const;

        void refresh_mirror_after_write() const;
    };

} // namespace orangutan
