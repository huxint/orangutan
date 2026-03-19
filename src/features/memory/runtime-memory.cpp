#include "features/memory/runtime-memory.hpp"

#include "features/memory/memory-search.hpp"

#include <algorithm>
#include <array>

namespace orangutan {
namespace {

std::string make_journal_key(const std::string &summary) {
    return "journal." + std::to_string(std::hash<std::string>{}(summary));
}

} // namespace

RuntimeMemory::RuntimeMemory(MemoryStore &store, RuntimeMemoryContext context, MemoryMirror mirror)
: store_(store),
  context_(std::move(context)),
  mirror_(std::move(mirror)) {}

void RuntimeMemory::remember(const std::string &key, const std::string &content, const std::string &category, const std::string &source, double importance) {
    store_.remember(key, content, category, context_.scope, source, importance);
    refresh_mirror_after_write();
}

void RuntimeMemory::update(const std::string &key, const std::string &content, const std::string &category, bool merge, const std::string &source,
                           double importance) {
    store_.update(key, content, category, context_.scope, merge, source, importance);
    refresh_mirror_after_write();
}

bool RuntimeMemory::forget(const std::string &key) {
    const auto forgot = store_.forget(key, context_.scope);
    if (forgot) {
        refresh_mirror_after_write();
    }
    return forgot;
}

std::vector<MemoryRecord> RuntimeMemory::search(const std::string &query, size_t limit) {
    return store_.search(query, context_.scope, limit);
}

std::vector<MemoryRecord> RuntimeMemory::prompt_memories(const std::string &query, size_t limit) {
    auto records = search(query, std::max<size_t>(limit, 8));
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

std::string RuntimeMemory::recall(const std::string &query, size_t limit) {
    return store_.recall(query, context_.scope, limit);
}

std::vector<std::pair<std::string, std::string>> RuntimeMemory::recall_by_category(const std::string &category, size_t limit) {
    return store_.recall_by_category(category, context_.scope, limit);
}

std::vector<MemoryRecord> RuntimeMemory::list(const std::string &category, size_t limit) {
    return store_.list(context_.scope, category, limit);
}

MemoryStats RuntimeMemory::stats() {
    return store_.stats(context_.scope);
}

size_t RuntimeMemory::auto_capture(const std::string &text, const std::string &source) {
    const auto stored = store_.auto_capture(text, context_.scope, source);
    if (stored > 0) {
        refresh_mirror_after_write();
    }
    return stored;
}

MemoryMirrorRefreshResult RuntimeMemory::refresh_mirror() const {
    return mirror_.refresh_snapshot(context_, durable_records());
}

JournalStoreResult RuntimeMemory::store_journal_summary(const std::string &summary, const std::string &source) {
    const auto trimmed_summary = memory_detail::trim_copy(summary);
    if (trimmed_summary.empty()) {
        return {.stored = false, .mirrored = false, .status = "Journal summary is empty.", .key = {}};
    }

    const auto key = make_journal_key(trimmed_summary);
    store_.remember(key, trimmed_summary, "journal", context_.scope, source.empty() ? std::string{"session:journal"} : source, 0.35);
    const auto journal_result = mirror_.append_daily_journal(context_, trimmed_summary);
    refresh_mirror_after_write();
    return {
        .stored = true,
        .mirrored = journal_result.mirrored,
        .status = journal_result.mirrored ? std::string{"Stored journal summary."} : journal_result.status,
        .key = key,
    };
}

bool RuntimeMemory::query_requests_journal(std::string_view query) {
    static constexpr auto normalized_keywords = std::to_array<std::string_view>({
        "journal",
        "diary",
        "previous session",
        "last session",
        "recent session",
    });
    static constexpr auto raw_keywords = std::to_array<std::string_view>({
        "日记",
        "会话",
    });

    const auto trimmed_query = memory_detail::trim_copy(std::string(query));
    const auto normalized = memory_detail::normalize_ascii(trimmed_query);
    const auto trimmed_view = std::string_view(trimmed_query);
    return std::ranges::any_of(normalized_keywords, [&normalized](std::string_view keyword) {
               return normalized.contains(keyword);
           }) ||
           std::ranges::any_of(raw_keywords, [trimmed_view](std::string_view keyword) {
               return trimmed_view.contains(keyword);
           });
}

std::vector<MemoryRecord> RuntimeMemory::durable_records() const {
    auto records = store_.list(context_.scope, {}, 200);
    std::erase_if(records, [](const MemoryRecord &record) {
        return record.category == "journal";
    });
    return records;
}

void RuntimeMemory::refresh_mirror_after_write() {
    (void)refresh_mirror();
}

} // namespace orangutan
