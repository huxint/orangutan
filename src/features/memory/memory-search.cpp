#include "features/memory/memory-search.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace orangutan::memory_detail {

namespace {

double substring_score(std::string_view haystack, std::string_view needle, double score) {
    return haystack.contains(needle) ? score : 0.0;
}

double exact_score(std::string_view left, std::string_view right, double score) {
    return left == right ? score : 0.0;
}

double token_overlap_score(const std::vector<std::string> &tokens, std::string_view haystack, double per_token_score) {
    double score = 0.0;
    for (const auto &token : tokens) {
        score += substring_score(haystack, token, per_token_score);
    }
    return score;
}

} // namespace

std::string trim_copy(std::string value) {
    const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> split_memory_fragments(const std::string &value) {
    std::vector<std::string> fragments;
    std::string current;

    const auto flush = [&] {
        auto trimmed = trim_copy(current);
        if (!trimmed.empty()) {
            fragments.push_back(std::move(trimmed));
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

std::string normalize_ascii(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
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

double score_memory_match(const MemoryRecord &record, const std::string &query) {
    const auto trimmed_query = trim_copy(query);
    if (trimmed_query.empty()) {
        return 0.0;
    }

    const auto normalized_query = normalize_ascii(trimmed_query);
    const auto normalized_key = normalize_ascii(record.key);
    const auto normalized_content = normalize_ascii(record.content);
    const auto normalized_category = normalize_ascii(record.category);
    const auto query_tokens = tokenize_ascii_words(trimmed_query);
    const bool query_has_non_ascii = contains_non_ascii(trimmed_query);

    double score = (record.importance * 10.0) + static_cast<double>(std::min(record.access_count, 8));

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
    std::ostringstream out;
    for (const auto &record : records) {
        out << '[' << record.category << ':' << record.key << "] " << record.content;
        if (!record.source.empty()) {
            out << " {source=" << record.source << '}';
        }
        out << '\n';
    }
    return out.str();
}

std::string merge_memory_content(const std::string &existing, const std::string &incoming) {
    auto trimmed_existing = trim_copy(existing);
    auto trimmed_incoming = trim_copy(incoming);
    if (trimmed_existing.empty()) {
        return trimmed_incoming;
    }
    if (trimmed_incoming.empty()) {
        return trimmed_existing;
    }
    if (trimmed_existing == trimmed_incoming) {
        return trimmed_existing;
    }
    if (trimmed_existing.contains(trimmed_incoming)) {
        return trimmed_existing;
    }
    if (trimmed_incoming.contains(trimmed_existing)) {
        return trimmed_incoming;
    }

    auto fragments = split_memory_fragments(trimmed_existing);
    const auto incoming_fragments = split_memory_fragments(trimmed_incoming);
    std::set<std::string> seen;
    for (const auto &fragment : fragments) {
        seen.insert(normalize_ascii(fragment));
    }
    for (const auto &fragment : incoming_fragments) {
        if (seen.insert(normalize_ascii(fragment)).second) {
            fragments.push_back(fragment);
        }
    }

    std::ostringstream merged;
    for (size_t index = 0; index < fragments.size(); ++index) {
        if (index > 0) {
            merged << "\n";
        }
        merged << fragments[index];
    }
    return merged.str();
}

std::optional<std::string> build_fts_query(const std::string &query) {
    const auto tokens = tokenize_ascii_words(query);
    if (tokens.empty()) {
        return std::nullopt;
    }

    std::ostringstream out;
    for (size_t index = 0; index < tokens.size(); ++index) {
        if (index > 0) {
            out << " OR ";
        }
        out << tokens[index];
    }
    return out.str();
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
                          const std::string &source, double importance) {
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
    (void)stmt.step();
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
        (void)stmt.step();
        stmt.reset();
    }
}

} // namespace orangutan::memory_detail
