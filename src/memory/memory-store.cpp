#include "memory/memory-store.hpp"

#include "memory/memory-schema.hpp"
#include "memory/memory-search.hpp"
#include "memory/memory-age.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>

namespace orangutan::memory {

    MemoryStore::MemoryStore()
    : MemoryStore(memory_detail::default_db_path()) {}

    MemoryStore::MemoryStore(const std::filesystem::path &db_path)
    : db_(db_path) {
        ensure_schema();
    }

    MemoryStore::~MemoryStore() = default;

    void MemoryStore::ensure_schema() {
        memory_detail::create_current_schema(db_);
        fts_enabled_ = memory_detail::enable_fts_if_available(db_);
    }

    void MemoryStore::remember(std::string_view key, std::string_view content, std::string_view category, memory_type type, std::string_view scope, std::string_view source,
                               base::f64 importance) {
        std::scoped_lock lock(mutex_);
        const auto type_str = std::string(magic_enum::enum_name(type));
        memory_detail::upsert_memory_record(db_, scope, key, content, category, type_str, source, importance);
    }

    void MemoryStore::update(std::string_view key, std::string_view content, std::string_view category, memory_type type, std::string_view scope, bool merge,
                             std::string_view source, base::f64 importance) {
        std::scoped_lock lock(mutex_);
        const auto existing = memory_detail::fetch_memory_by_key(db_, scope, key);

        auto final_content = std::string(content);
        auto final_category = std::string(category);
        auto final_type = std::string(magic_enum::enum_name(type));
        auto final_source = std::string(source);
        auto final_importance = importance;

        if (existing.has_value()) {
            if (merge) {
                final_content = memory_detail::merge_memory_content(existing->content, content);
            }
            if (category.empty()) {
                final_category = existing->category;
            }
            if (type == memory_type::user && !category.empty()) {
                // If type wasn't explicitly set but category was, infer from category
                final_type = std::string(magic_enum::enum_name(infer_memory_type(final_category)));
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

        memory_detail::upsert_memory_record(db_, scope, key, final_content, final_category, final_type, final_source, final_importance);
    }

    std::vector<MemoryRecord> MemoryStore::search(std::string_view query, std::string_view scope, std::size_t limit) {
        std::scoped_lock lock(mutex_);
        const auto trimmed_query = static_cast<std::string>(utils::trim_copy(query));
        if (trimmed_query.empty()) {
            return {};
        }

        const auto effective_limit = limit == 0 ? memory_detail::DEFAULT_SEARCH_LIMIT : limit;
        std::unordered_map<int, base::f64> fts_bonus_by_id;
        if (fts_enabled_) {
            if (const auto fts_query = memory_detail::build_fts_query(trimmed_query); fts_query.has_value()) {
                auto fts_records = std::vector<MemoryRecord>{};
                auto fts_stmt = db_.query("SELECT m.id, m.memory_key, m.content, m.category, m.type, m.scope, m.source, m.updated_at, m.importance, m.access_count "
                                          "FROM memories_fts JOIN memories m ON m.id = memories_fts.rowid "
                                          "WHERE memories_fts MATCH ? AND m.scope = ? ORDER BY rank LIMIT 64");
                fts_stmt.bind(*fts_query, scope).for_each([&](const sqlite::Row &row) {
                    fts_records.push_back(memory_detail::read_memory_record(row));
                });
                for (std::size_t index = 0; index < fts_records.size(); ++index) {
                    fts_bonus_by_id.insert_or_assign(fts_records[index].id, 80.0 - static_cast<base::f64>(index));
                }
            }
        }

        auto records = std::vector<MemoryRecord>{};
        auto stmt = db_.query("SELECT id, memory_key, content, category, type, scope, source, updated_at, importance, access_count "
                              "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
        stmt.bind(scope, static_cast<int>(memory_detail::SEARCH_SCAN_LIMIT)).for_each([&](const sqlite::Row &row) {
            records.push_back(memory_detail::read_memory_record(row));
        });
        struct RankedRecord {
            MemoryRecord record;
            base::f64 score = 0.0;
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

        const auto category_cap = std::max<std::size_t>(1, (effective_limit + 1) / 2);
        std::set<std::string> selected_keys;
        std::unordered_map<std::string, std::size_t> category_counts;
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

    std::string MemoryStore::recall(std::string_view query, std::string_view scope, std::size_t limit) {
        return memory_detail::format_records(search(query, scope, limit));
    }

    std::vector<std::pair<std::string, std::string>> MemoryStore::recall_by_category(std::string_view category, std::string_view scope, std::size_t limit) {
        auto records = list(scope, category, limit);

        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(records.size());
        for (const auto &record : records) {
            entries.emplace_back(record.key, record.content);
        }
        return entries;
    }

    std::vector<MemoryRecord> MemoryStore::list(std::string_view scope, std::string_view category, std::size_t limit) {
        std::scoped_lock lock(mutex_);
        const auto capped_limit = static_cast<int>(limit == 0 ? memory_detail::DEFAULT_LIST_LIMIT : limit);

        auto records = std::vector<MemoryRecord>{};
        if (category.empty()) {
            auto stmt = db_.query("SELECT id, memory_key, content, category, type, scope, source, updated_at, importance, access_count "
                                  "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
            stmt.bind(scope, capped_limit).for_each([&](const sqlite::Row &row) {
                records.push_back(memory_detail::read_memory_record(row));
            });
        } else {
            auto stmt = db_.query("SELECT id, memory_key, content, category, type, scope, source, updated_at, importance, access_count "
                                  "FROM memories WHERE scope = ? AND category = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
            stmt.bind(scope, category, capped_limit).for_each([&](const sqlite::Row &row) {
                records.push_back(memory_detail::read_memory_record(row));
            });
        }
        return records;
    }

    MemoryStats MemoryStore::stats(std::string_view scope) {
        std::scoped_lock lock(mutex_);
        const auto [total, categories, auto_entries, manual_entries, journal_entries] = db_.query(
            "SELECT COUNT(*), COUNT(DISTINCT category), "
            "COALESCE(SUM(CASE WHEN source LIKE 'auto:%' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN source = 'manual' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN category = 'journal' THEN 1 ELSE 0 END), 0) "
            "FROM memories WHERE scope = ?")
                                                                                          .bind(scope)
                                                                                          .one<std::tuple<int, int, int, int, int>>();

        return MemoryStats{
            .total = total,
            .categories = categories,
            .manual_entries = manual_entries,
            .auto_entries = auto_entries,
            .journal_entries = journal_entries,
        };
    }

    bool MemoryStore::forget(std::string_view key, std::string_view scope) {
        std::scoped_lock lock(mutex_);
        db_.exec("DELETE FROM memories WHERE scope = ? AND memory_key = ?").bind(scope, key).run();
        return db_.changes() > 0;
    }

    std::string MemoryStore::dump_all(std::string_view scope, std::size_t limit) {
        return memory_detail::format_records(list(scope, {}, limit));
    }

    std::size_t MemoryStore::consolidate(std::string_view scope, std::size_t max_per_scope, int stale_days, base::f64 stale_importance_threshold) {
        std::scoped_lock lock(mutex_);

        // Phase 1: Prune stale, low-importance, non-journal memories.
        std::size_t pruned = 0;
        {
            auto records = std::vector<MemoryRecord>{};
            auto stmt = db_.query("SELECT id, memory_key, content, category, type, scope, source, updated_at, importance, access_count "
                                  "FROM memories WHERE scope = ? AND category != 'journal' "
                                  "ORDER BY importance ASC, access_count ASC, updated_at ASC LIMIT 500");
            stmt.bind(scope).for_each([&](const sqlite::Row &row) {
                records.push_back(memory_detail::read_memory_record(row));
            });

            sqlite::Statement del(db_, "DELETE FROM memories WHERE id = ?");
            for (const auto &record : records) {
                const auto age = memory_age_days(record.updated_at);
                if (age >= stale_days && record.importance <= stale_importance_threshold && record.access_count <= 1) {
                    del.clear_bindings();
                    del.bind(1, record.id);
                    static_cast<void>(del.step());
                    del.reset();
                    ++pruned;
                }
            }
        }

        // Phase 2: Enforce per-scope limit (keep most important/recent, drop the rest).
        {
            const auto total = static_cast<std::size_t>(db_.query("SELECT COUNT(*) FROM memories WHERE scope = ? AND category != 'journal'")
                                                            .bind(scope)
                                                            .one<int>());
            if (total > max_per_scope) {
                const auto excess = static_cast<int>(total - max_per_scope);
                db_.exec("DELETE FROM memories WHERE id IN ("
                         "SELECT id FROM memories WHERE scope = ? AND category != 'journal' "
                         "ORDER BY importance ASC, access_count ASC, updated_at ASC LIMIT ?)")
                    .bind(scope, excess)
                    .run();
                pruned += static_cast<std::size_t>(db_.changes());
            }
        }

        return pruned;
    }

    std::string MemoryStore::manifest(std::string_view scope, std::size_t limit) {
        auto records = list(scope, {}, limit);
        std::erase_if(records, [](const MemoryRecord &record) {
            return record.category == "journal";
        });
        return memory_detail::format_memory_manifest(records);
    }

} // namespace orangutan::memory
