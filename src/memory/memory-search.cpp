#include "memory/memory-search.hpp"
#include "infra/format.hpp"
#include "infra/string.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <uni_algo/case.h>

namespace orangutan::memory_detail {

    namespace {

        base::f64 substring_score(std::string_view haystack, std::string_view needle, base::f64 score) {
            return haystack.contains(needle) ? score : 0.0;
        }

        base::f64 exact_score(std::string_view left, std::string_view right, base::f64 score) {
            return left == right ? score : 0.0;
        }

        base::f64 token_overlap_score(const std::vector<std::string> &tokens, std::string_view haystack, base::f64 per_token_score) {
            base::f64 score = 0.0;
            for (const auto &token : tokens) {
                score += substring_score(haystack, token, per_token_score);
            }
            return score;
        }

    } // namespace

    std::vector<std::string> split_memory_fragments(std::string_view value) {
        std::vector<std::string> fragments;
        std::string current;

        const auto flush = [&] {
            const auto trimmed = utils::trim_copy(current);
            if (!trimmed.empty()) {
                fragments.emplace_back(static_cast<std::string>(trimmed));
            }
            current.clear();
        };

        for (const char ch : value) {
            if (ch == '\n' || ch == ';') {
                flush();
                continue;
            }
            current.push_back(ch);
        }
        flush();
        return fragments;
    }
    std::vector<std::string> tokenize_ascii_words(std::string_view value) {
        std::vector<std::string> tokens;
        std::string current;

        for (const unsigned char ch : value) {
            if (std::isalnum(ch) != 0) {
                current.push_back(static_cast<char>(std::tolower(ch)));
                continue;
            }

            if (current.size() >= 2) {
                tokens.push_back(current);
            }
            current.clear();
        }

        if (current.size() >= 2) {
            tokens.push_back(current);
        }

        std::ranges::sort(tokens);
        tokens.erase(std::ranges::unique(tokens).begin(), tokens.end());
        return tokens;
    }

    bool contains_non_ascii(std::string_view value) {
        return std::ranges::any_of(value, [](unsigned char ch) {
            return ch > 0x7F;
        });
    }

    base::f64 score_memory_match(const MemoryRecord &record, const std::string &query) {
        auto trimmed_query = utils::trim_copy(query);
        if (trimmed_query.empty()) {
            return 0.0;
        }

        const auto normalized_query = una::cases::to_lowercase_utf8(trimmed_query);
        const auto normalized_key = una::cases::to_lowercase_utf8(record.key);
        const auto normalized_content = una::cases::to_lowercase_utf8(record.content);
        const auto normalized_category = una::cases::to_lowercase_utf8(record.category);
        const auto query_tokens = tokenize_ascii_words(trimmed_query);
        const bool query_has_non_ascii = contains_non_ascii(trimmed_query);

        base::f64 score = (record.importance * 10.0) + static_cast<base::f64>(std::min(record.access_count, 8));

        if (!normalized_query.empty()) {
            score += exact_score(normalized_key, normalized_query, 100.0);
            score += exact_score(normalized_category, normalized_query, 35.0);
            score += substring_score(normalized_key, normalized_query, 30.0);
            score += substring_score(normalized_content, normalized_query, 18.0);
            score += substring_score(normalized_category, normalized_query, 8.0);
        }

        if (query_has_non_ascii) {
            score += substring_score(record.key, trimmed_query, 28.0);
            score += substring_score(record.content, trimmed_query, 18.0);
            score += substring_score(record.category, trimmed_query, 8.0);
        }

        score += token_overlap_score(query_tokens, normalized_key, 12.0);
        score += token_overlap_score(query_tokens, normalized_category, 6.0);
        score += token_overlap_score(query_tokens, normalized_content, 5.0);

        return score;
    }

    std::string format_records(const std::vector<MemoryRecord> &records) {
        std::string out;
        for (const auto &record : records) {
            append(out, "[{}:{}] {}", record.category, record.key, record.content);
            if (!record.source.empty()) {
                append(out, " {{source={}}}", record.source);
            }
            out.push_back('\n');
        }
        return out;
    }

    std::string merge_memory_content(const std::string &existing, const std::string &incoming) {
        const auto trimmed_existing = utils::trim_copy(existing);
        const auto trimmed_incoming = utils::trim_copy(incoming);
        if (trimmed_existing.empty() || trimmed_incoming.contains(trimmed_existing)) {
            return static_cast<std::string>(trimmed_incoming);
        }
        if (trimmed_incoming.empty() || trimmed_existing == trimmed_incoming || trimmed_existing.contains(trimmed_incoming)) {
            return static_cast<std::string>(trimmed_existing);
        }

        auto fragments = split_memory_fragments(trimmed_existing);
        const auto incoming_fragments = split_memory_fragments(trimmed_incoming);
        auto seen = fragments | std::ranges::views::transform([](std::string_view fragment) {
                        return una::cases::to_lowercase_utf8(fragment);
                    }) |
                    std::ranges::to<std::set<std::string>>();

        for (const auto &fragment : incoming_fragments) {
            if (seen.insert(una::cases::to_lowercase_utf8(fragment)).second) {
                fragments.push_back(fragment);
            }
        }

        return fragments | std::ranges::views::join_with(std::string_view{"\n"}) | std::ranges::to<std::string>();
    }

    std::optional<std::string> build_fts_query(const std::string &query) {
        const auto tokens = tokenize_ascii_words(query);
        if (tokens.empty()) {
            return std::nullopt;
        }

        std::string out;
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (index > 0) {
                out.append(" OR ");
            }
            out.append(tokens[index]);
        }
        return out;
    }

    std::vector<MemoryRecord> collect_records(sqlite::Statement &stmt) {
        std::vector<MemoryRecord> records;
        while (stmt.step()) {
            records.push_back({
                .id = stmt.column_int(0),
                .key = stmt.column_text(1),
                .content = stmt.column_text(2),
                .category = stmt.column_text(3),
                .scope = stmt.column_text(4),
                .source = stmt.column_text(5),
                .updated_at = stmt.column_text(6),
                .importance = stmt.column_double(7),
                .access_count = stmt.column_int(8),
            });
        }
        return records;
    }

    std::optional<MemoryRecord> fetch_memory_by_key(sqlite::Database &db, const std::string &scope, const std::string &key) {
        sqlite::Statement stmt(db, "SELECT id, memory_key, content, category, scope, source, updated_at, importance, access_count "
                                   "FROM memories WHERE scope = ? AND memory_key = ? LIMIT 1");
        stmt.bind_text(1, scope);
        stmt.bind_text(2, key);
        auto rows = collect_records(stmt);
        if (rows.empty()) {
            return std::nullopt;
        }
        return rows.front();
    }

    void upsert_memory_record(sqlite::Database &db, const std::string &scope, const std::string &key, const std::string &content, const std::string &category,
                              const std::string &source, base::f64 importance) {
        sqlite::Statement stmt(db, "INSERT INTO memories (scope, memory_key, content, category, source, importance, created_at, updated_at, last_accessed_at) "
                                   "VALUES (?, ?, ?, ?, ?, ?, datetime('now'), datetime('now'), NULL) "
                                   "ON CONFLICT(scope, memory_key) DO UPDATE SET "
                                   "content = excluded.content, "
                                   "category = excluded.category, "
                                   "source = excluded.source, "
                                   "importance = excluded.importance, "
                                   "updated_at = datetime('now')");
        stmt.bind_text(1, scope);
        stmt.bind_text(2, key);
        stmt.bind_text(3, content);
        stmt.bind_text(4, category.empty() ? std::string{"general"} : category);
        stmt.bind_text(5, source.empty() ? std::string{"manual"} : source);
        stmt.bind_double(6, importance);
        static_cast<void>(stmt.step());
    }

    std::vector<MemoryRecord> dedupe_records_by_key(std::vector<MemoryRecord> records) {
        std::set<std::pair<std::string, std::string>> seen;
        std::vector<MemoryRecord> deduped;
        deduped.reserve(records.size());
        for (auto &record : records) {
            const auto token = std::pair{record.scope, record.key};
            if (seen.insert(token).second) {
                deduped.push_back(std::move(record));
            }
        }
        return deduped;
    }

    void touch_records(sqlite::Database &db, const std::vector<MemoryRecord> &records) {
        sqlite::Statement stmt(db, "UPDATE memories SET access_count = access_count + 1, last_accessed_at = datetime('now') WHERE id = ?");
        for (const auto &record : records) {
            stmt.bind_int(1, record.id);
            static_cast<void>(stmt.step());
            stmt.reset();
        }
    }

} // namespace orangutan::memory_detail
