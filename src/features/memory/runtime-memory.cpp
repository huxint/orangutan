#include "features/memory/runtime-memory.hpp"

#include "features/memory/memory-search.hpp"

#include <algorithm>

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

    records.erase(std::remove_if(records.begin(), records.end(), [](const MemoryRecord &record) {
                      return record.category == "journal";
                  }),
                  records.end());
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
    const auto normalized = memory_detail::normalize_ascii(memory_detail::trim_copy(std::string(query)));
    return normalized.find("journal") != std::string::npos || normalized.find("diary") != std::string::npos ||
           normalized.find("previous session") != std::string::npos || normalized.find("last session") != std::string::npos ||
           normalized.find("recent session") != std::string::npos || std::string(query).find("日记") != std::string::npos ||
           std::string(query).find("会话") != std::string::npos;
}

std::vector<MemoryRecord> RuntimeMemory::durable_records() const {
    auto records = store_.list(context_.scope, {}, 200);
    records.erase(std::remove_if(records.begin(), records.end(), [](const MemoryRecord &record) {
                      return record.category == "journal";
                  }),
                  records.end());
    return records;
}

void RuntimeMemory::refresh_mirror_after_write() {
    (void)refresh_mirror();
}

} // namespace orangutan
