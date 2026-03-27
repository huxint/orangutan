#include "features/memory/memory.hpp"

#include "features/memory/memory-extract.hpp"
#include "features/memory/memory-schema.hpp"
#include "features/memory/memory-search.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>

namespace orangutan {

    MemoryStore::MemoryStore()
    : MemoryStore(memory_detail::default_db_path()) {}

    MemoryStore::MemoryStore(const std::filesystem::path &db_path)
    : db_(db_path) {
        ensure_schema();
    }

    MemoryStore::~MemoryStore() = default;

    void MemoryStore::ensure_schema() {
        if (!db_.table_exists("memories")) {
            memory_detail::create_current_schema(db_);
            fts_enabled_ = memory_detail::enable_fts_if_available(db_);
            return;
        }

        const auto schema = memory_detail::inspect_memory_schema(db_);
        if (!schema.has_scope || !schema.has_memory_key || !schema.has_source || !schema.has_importance || !schema.has_access_count || !schema.has_last_accessed_at) {
            memory_detail::migrate_legacy_memories(db_);
        }

        memory_detail::create_current_schema(db_);
        fts_enabled_ = memory_detail::enable_fts_if_available(db_);
    }

    void MemoryStore::remember(const std::string &key, const std::string &content, const std::string &category, const std::string &scope, const std::string &source,
                               double importance) {
        std::scoped_lock lock(mutex_);
        memory_detail::upsert_memory_record(db_, scope, key, content, category, source, importance);
    }

    void MemoryStore::update(const std::string &key, const std::string &content, const std::string &category, const std::string &scope, bool merge, const std::string &source,
                             double importance) {
        std::scoped_lock lock(mutex_);
        const auto existing = memory_detail::fetch_memory_by_key(db_, scope, key);

        auto final_content = content;
        auto final_category = category;
        auto final_source = source;
        auto final_importance = importance;

        if (existing.has_value()) {
            if (merge) {
                final_content = memory_detail::merge_memory_content(existing->content, content);
            }
            if (category.empty()) {
                final_category = existing->category;
            }
            if (source.empty()) {
                final_source = existing->source;
            }
            final_importance = std::max(existing->importance, importance);
        }

        if (final_category.empty()) {
            final_category = "general";
        }
        if (final_source.empty()) {
            final_source = "manual";
        }

        memory_detail::upsert_memory_record(db_, scope, key, final_content, final_category, final_source, final_importance);
    }

    std::vector<MemoryRecord> MemoryStore::search(const std::string &query, const std::string &scope, size_t limit) {
        std::scoped_lock lock(mutex_);
        const auto trimmed_query = memory_detail::trim_copy(query);
        if (trimmed_query.empty()) {
            return {};
        }

        const auto effective_limit = limit == 0 ? memory_detail::default_search_limit : limit;
        std::unordered_map<int, double> fts_bonus_by_id;
        if (fts_enabled_) {
            if (const auto fts_query = memory_detail::build_fts_query(trimmed_query); fts_query.has_value()) {
                sqlite::Statement fts_stmt(db_, "SELECT m.id, m.memory_key, m.content, m.category, m.scope, m.source, m.updated_at, m.importance, m.access_count "
                                                "FROM memories_fts JOIN memories m ON m.id = memories_fts.rowid "
                                                "WHERE memories_fts MATCH ? AND m.scope = ? ORDER BY rank LIMIT 64");
                fts_stmt.bind_text(1, *fts_query);
                fts_stmt.bind_text(2, scope);
                auto fts_records = memory_detail::collect_records(fts_stmt);
                for (size_t index = 0; index < fts_records.size(); ++index) {
                    fts_bonus_by_id.insert_or_assign(fts_records[index].id, 80.0 - static_cast<double>(index));
                }
            }
        }

        sqlite::Statement stmt(db_, "SELECT id, memory_key, content, category, scope, source, updated_at, importance, access_count "
                                    "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
        stmt.bind_text(1, scope);
        stmt.bind_int(2, static_cast<int>(memory_detail::search_scan_limit));

        auto records = memory_detail::collect_records(stmt);
        struct RankedRecord {
            MemoryRecord record;
            double score = 0.0;
        };

        std::vector<RankedRecord> ranked;
        ranked.reserve(records.size());
        for (auto &record : records) {
            auto score = memory_detail::score_memory_match(record, trimmed_query);
            if (const auto it = fts_bonus_by_id.find(record.id); it != fts_bonus_by_id.end()) {
                score += it->second;
            }
            if (score > 0.0) {
                ranked.push_back({.record = std::move(record), .score = score});
            }
        }

        std::ranges::sort(ranked, [](const RankedRecord &left, const RankedRecord &right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.record.updated_at != right.record.updated_at) {
                return left.record.updated_at > right.record.updated_at;
            }
            return left.record.id > right.record.id;
        });

        const auto category_cap = std::max<size_t>(1, (effective_limit + 1) / 2);
        std::set<std::string> selected_keys;
        std::unordered_map<std::string, size_t> category_counts;
        std::vector<MemoryRecord> selected;
        selected.reserve(std::min(effective_limit, ranked.size()));
        for (const auto &entry : ranked) {
            if (selected_keys.contains(entry.record.key)) {
                continue;
            }
            auto &category_count = category_counts[entry.record.category];
            if (category_count >= category_cap) {
                continue;
            }
            selected.push_back(entry.record);
            selected_keys.insert(entry.record.key);
            ++category_count;
            if (selected.size() >= effective_limit) {
                break;
            }
        }

        for (const auto &entry : ranked) {
            if (selected.size() >= effective_limit) {
                break;
            }
            if (selected_keys.contains(entry.record.key)) {
                continue;
            }
            selected.push_back(entry.record);
            selected_keys.insert(entry.record.key);
        }

        memory_detail::touch_records(db_, selected);
        return selected;
    }

    std::string MemoryStore::recall(const std::string &query, const std::string &scope, size_t limit) {
        return memory_detail::format_records(search(query, scope, limit));
    }

    std::vector<std::pair<std::string, std::string>> MemoryStore::recall_by_category(const std::string &category, const std::string &scope, size_t limit) {
        auto records = list(scope, category, limit);

        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(records.size());
        for (const auto &record : records) {
            entries.emplace_back(record.key, record.content);
        }
        return entries;
    }

    std::vector<MemoryRecord> MemoryStore::list(const std::string &scope, const std::string &category, size_t limit) {
        std::scoped_lock lock(mutex_);
        const auto capped_limit = static_cast<int>(limit == 0 ? memory_detail::default_list_limit : limit);

        sqlite::Statement stmt(db_, category.empty() ? "SELECT id, memory_key, content, category, scope, source, updated_at, importance, access_count "
                                                       "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?"
                                                     : "SELECT id, memory_key, content, category, scope, source, updated_at, importance, access_count "
                                                       "FROM memories WHERE scope = ? AND category = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
        stmt.bind_text(1, scope);
        if (category.empty()) {
            stmt.bind_int(2, capped_limit);
        } else {
            stmt.bind_text(2, category);
            stmt.bind_int(3, capped_limit);
        }

        return memory_detail::collect_records(stmt);
    }

    MemoryStats MemoryStore::stats(const std::string &scope) {
        std::scoped_lock lock(mutex_);
        sqlite::Statement totals(db_, "SELECT COUNT(*), COUNT(DISTINCT category), "
                                      "SUM(CASE WHEN source LIKE 'auto:%' THEN 1 ELSE 0 END), "
                                      "SUM(CASE WHEN source = 'manual' THEN 1 ELSE 0 END), "
                                      "SUM(CASE WHEN category = 'journal' THEN 1 ELSE 0 END) "
                                      "FROM memories WHERE scope = ?");
        totals.bind_text(1, scope);

        MemoryStats result;
        if (totals.step()) {
            result.total = totals.column_int(0);
            result.categories = totals.column_int(1);
            result.auto_entries = totals.column_int(2);
            result.manual_entries = totals.column_int(3);
            result.journal_entries = totals.column_int(4);
        }
        return result;
    }

    bool MemoryStore::forget(const std::string &key, const std::string &scope) {
        std::scoped_lock lock(mutex_);
        sqlite::Statement stmt(db_, "DELETE FROM memories WHERE scope = ? AND memory_key = ?");
        stmt.bind_text(1, scope);
        stmt.bind_text(2, key);
        static_cast<void>(stmt.step());
        return db_.changes() > 0;
    }

    std::string MemoryStore::dump_all(const std::string &scope, size_t limit) {
        return memory_detail::format_records(list(scope, {}, limit));
    }

    size_t MemoryStore::auto_capture(const std::string &text, const std::string &scope, const std::string &source) {
        if (!memory_detail::should_attempt_auto_capture(text)) {
            return 0;
        }

        const auto candidates = memory_detail::extract_auto_candidates(text);
        size_t stored = 0;
        for (const auto &candidate : candidates) {
            update(candidate.key, candidate.content, candidate.category, scope, memory_detail::should_merge_auto_candidate(candidate.key), source, candidate.importance);
            ++stored;
        }
        return stored;
    }

} // namespace orangutan
