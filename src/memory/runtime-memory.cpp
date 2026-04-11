#include "memory/runtime-memory.hpp"

#include "utils/string.hpp"

#include <uni_algo/case.h>
#include <algorithm>
#include <array>

namespace orangutan::memory {
    namespace {

        std::string make_journal_key(std::string_view summary) {
            return "journal." + std::to_string(std::hash<std::string_view>{}(summary));
        }

    } // namespace

    RuntimeMemory::RuntimeMemory(MemoryStore &store, bootstrap::RuntimeMemoryContext context, MemoryMirror mirror)
    : store_(&store),
      context_(std::move(context)),
      mirror_(mirror) {}

    void RuntimeMemory::remember(std::string_view key, std::string_view content, std::string_view category, memory_type type, std::string_view source, base::f64 importance) {
        store_->remember(key, content, category, type, context_.scope, source, importance);
        refresh_mirror_after_write();
    }

    void RuntimeMemory::update(std::string_view key, std::string_view content, std::string_view category, memory_type type, bool merge, std::string_view source,
                               base::f64 importance) {
        store_->update(key, content, category, type, context_.scope, merge, source, importance);
        refresh_mirror_after_write();
    }

    bool RuntimeMemory::forget(std::string_view key) {
        const auto forgot = store_->forget(key, context_.scope);
        if (forgot) {
            refresh_mirror_after_write();
        }
        return forgot;
    }

    std::vector<MemoryRecord> RuntimeMemory::search(std::string_view query, std::size_t limit) {
        return store_->search(query, context_.scope, limit);
    }

    std::vector<MemoryRecord> RuntimeMemory::prompt_memories(std::string_view query, std::size_t limit) {
        auto records = search(query, std::max<std::size_t>(limit, 8));
        if (query_requests_journal(query)) {
            if (records.size() > limit) {
                records.resize(limit);
            }
            return records;
        }

        std::erase_if(records, [](const MemoryRecord &record) {
            return record.category == "journal";
        });
        if (records.size() > limit) {
            records.resize(limit);
        }
        return records;
    }

    std::string RuntimeMemory::recall(std::string_view query, std::size_t limit) {
        return store_->recall(query, context_.scope, limit);
    }

    std::vector<std::pair<std::string, std::string>> RuntimeMemory::recall_by_category(std::string_view category, std::size_t limit) {
        return store_->recall_by_category(category, context_.scope, limit);
    }

    std::vector<MemoryRecord> RuntimeMemory::list(std::string_view category, std::size_t limit) {
        return store_->list(context_.scope, category, limit);
    }

    MemoryStats RuntimeMemory::stats() {
        return store_->stats(context_.scope);
    }

    MemoryMirrorRefreshResult RuntimeMemory::refresh_mirror() const {
        return MemoryMirror::refresh_snapshot(context_, durable_records());
    }

    JournalStoreResult RuntimeMemory::store_journal_summary(std::string_view summary, std::string_view source) {
        auto trimmed_summary = static_cast<std::string>(utils::trim_copy(summary));
        if (trimmed_summary.empty()) {
            return {.stored = false, .mirrored = false, .status = "Journal summary is empty.", .key = {}};
        }

        const auto key = make_journal_key(trimmed_summary);
        store_->remember(key, trimmed_summary, "journal", memory_type::project, context_.scope, source.empty() ? std::string{"session:journal"} : source, 0.35);
        const auto journal_result = MemoryMirror::append_daily_journal(context_, trimmed_summary);
        refresh_mirror_after_write();
        return {
            .stored = true,
            .mirrored = journal_result.mirrored,
            .status = journal_result.mirrored ? std::string{"Stored journal summary."} : journal_result.status,
            .key = key,
        };
    }

    bool RuntimeMemory::query_requests_journal(std::string_view query) {
        static constexpr auto NORMALIZED_KEYWORDS = std::to_array<std::string_view>({
            "journal",
            "diary",
            "previous session",
            "last session",
            "recent session",
        });
        static constexpr auto RAW_KEYWORDS = std::to_array<std::string_view>({
            "日记",
            "会话",
        });

        const auto trimmed_query = utils::trim_copy(query);
        const auto normalized = una::cases::to_lowercase_utf8(trimmed_query);
        return std::ranges::any_of(NORMALIZED_KEYWORDS,
                                   [&normalized](std::string_view keyword) {
                                       return normalized.contains(keyword);
                                   }) ||
               std::ranges::any_of(RAW_KEYWORDS, [trimmed_query](std::string_view keyword) {
                   return trimmed_query.contains(keyword);
               });
    }

    std::vector<MemoryRecord> RuntimeMemory::durable_records() const {
        auto records = store_->list(context_.scope, {}, 200);
        std::erase_if(records, [](const MemoryRecord &record) {
            return record.category == "journal";
        });
        return records;
    }

    void RuntimeMemory::refresh_mirror_after_write() const {
        static_cast<void>(refresh_mirror());
    }

    std::size_t RuntimeMemory::consolidate(std::size_t max_per_scope, int stale_days, base::f64 stale_importance_threshold) {
        const auto pruned = store_->consolidate(context_.scope, max_per_scope, stale_days, stale_importance_threshold);
        if (pruned > 0) {
            refresh_mirror_after_write();
        }
        return pruned;
    }

    std::string RuntimeMemory::manifest(std::size_t limit) {
        return store_->manifest(context_.scope, limit);
    }

} // namespace orangutan::memory
