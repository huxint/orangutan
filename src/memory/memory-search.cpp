#include "memory/memory-search.hpp"
#include "memory/memory-age.hpp"
#include "memory/memory-type.hpp"
#include "storage/sqlite-throwing.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <uni_algo/case.h>

namespace orangutan::memory::detail {

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

    base::f64 score_memory_match(const MemoryRecord &record, std::string_view query) {
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

        base::f64 match_score = 0.0;

        if (!normalized_query.empty()) {
            match_score += exact_score(normalized_key, normalized_query, 100.0);
            match_score += exact_score(normalized_category, normalized_query, 35.0);
            match_score += substring_score(normalized_key, normalized_query, 30.0);
            match_score += substring_score(normalized_content, normalized_query, 18.0);
            match_score += substring_score(normalized_category, normalized_query, 8.0);
        }

        if (query_has_non_ascii) {
            match_score += substring_score(record.key, trimmed_query, 28.0);
            match_score += substring_score(record.content, trimmed_query, 18.0);
            match_score += substring_score(record.category, trimmed_query, 8.0);
        }

        match_score += token_overlap_score(query_tokens, normalized_key, 12.0);
        match_score += token_overlap_score(query_tokens, normalized_category, 6.0);
        match_score += token_overlap_score(query_tokens, normalized_content, 5.0);

        if (match_score <= 0.0) {
            return 0.0;
        }

        return match_score + (record.importance * 10.0) + static_cast<base::f64>(std::min(record.access_count, 8));
    }

    std::string format_records(const std::vector<MemoryRecord> &records) {
        std::string out;
        for (const auto &record : records) {
            utils::format_to(out, "[{}:{}:{}] {}", magic_enum::enum_name(record.type), record.category, record.key, record.content);
            if (!record.source.empty()) {
                utils::format_to(out, " {{source={}}}", record.source);
            }
            out.push_back('\n');
        }
        return out;
    }

    std::string format_memory_manifest(const std::vector<MemoryRecord> &records) {
        std::string out;
        for (const auto &record : records) {
            utils::format_to(out, "- [{}] {} ({}): {}\n", magic_enum::enum_name(record.type), record.key, memory_age_text(record.updated_at),
                             record.content.substr(0, std::min<std::size_t>(record.content.size(), 80)));
        }
        return out;
    }

    std::string merge_memory_content(std::string_view existing, std::string_view incoming) {
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

    std::optional<std::string> build_fts_query(std::string_view query) {
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

    MemoryRecord read_memory_record(const sqlite::Row &row) {
        return MemoryRecord{
            .id = sqlite::unwrap(row.get<int>(0)),
            .key = sqlite::unwrap(row.get<std::string>(1)),
            .content = sqlite::unwrap(row.get<std::string>(2)),
            .category = sqlite::unwrap(row.get<std::string>(3)),
            .type = magic_enum::enum_cast<memory_type>(sqlite::unwrap(row.get<std::string>(4)), magic_enum::case_insensitive).value_or(memory_type::user),
            .scope = sqlite::unwrap(row.get<std::string>(5)),
            .source = sqlite::unwrap(row.get<std::string>(6)),
            .updated_at = sqlite::unwrap(row.get<std::string>(7)),
            .importance = sqlite::unwrap(row.get<base::f64>(8)),
            .access_count = sqlite::unwrap(row.get<int>(9)),
        };
    }

    std::optional<MemoryRecord> fetch_memory_by_key(sqlite::Database &db, std::string_view scope, std::string_view key) {
        auto records = std::vector<MemoryRecord>{};
        auto query = sqlite::unwrap(db.query("SELECT id, memory_key, content, category, type, scope, source, updated_at, importance, access_count "
                                             "FROM memories WHERE scope = ? AND memory_key = ? LIMIT 1"));
        sqlite::unwrap(query.bind(scope, key).for_each([&](const sqlite::Row &row) {
            records.push_back(read_memory_record(row));
        }));
        if (records.empty()) {
            return std::nullopt;
        }
        return records.front();
    }

    void upsert_memory_record(sqlite::Database &db, std::string_view scope, std::string_view key, std::string_view content, std::string_view category, std::string_view type,
                              std::string_view source, base::f64 importance) {
        sqlite::exec_bind(db,
                          "INSERT INTO memories (scope, memory_key, content, category, type, source, importance, created_at, updated_at, last_accessed_at) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, datetime('now'), datetime('now'), NULL) "
                          "ON CONFLICT(scope, memory_key) DO UPDATE SET "
                          "content = excluded.content, "
                          "category = excluded.category, "
                          "type = excluded.type, "
                          "source = excluded.source, "
                          "importance = excluded.importance, "
                          "updated_at = datetime('now')",
                          scope,
                          key,
                          content,
                          category.empty() ? std::string_view{"general"} : category,
                          type.empty() ? std::string_view{"user"} : type,
                          source.empty() ? std::string_view{"manual"} : source,
                          importance);
    }

    void touch_records(sqlite::Database &db, const std::vector<MemoryRecord> &records) {
        auto stmt = sqlite::prepare_or_throw(db, "UPDATE memories SET access_count = access_count + 1, last_accessed_at = datetime('now') WHERE id = ?");
        for (const auto &record : records) {
            sqlite::unwrap(stmt.clear_bindings());
            stmt.bind(1, record.id);
            sqlite::unwrap(stmt.step());
            sqlite::unwrap(stmt.reset());
        }
    }

} // namespace orangutan::memory::detail
