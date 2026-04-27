#include "memory/memory-store.hpp"

#include "memory/memory-schema.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <uni_algo/case.h>

namespace orangutan::sqlite {

    template <>
    struct RowMapper<memory::MemoryRecord> {
        static auto map(const Row &row) -> SqliteResult<memory::MemoryRecord> {
            auto columns = read_columns<int, std::string, std::string, std::string, std::string, std::string>(row);
            if (!columns) {
                return std::unexpected(columns.error());
            }
            auto &[id, key, content, kind, scope, updated_at] = *columns;
            return memory::MemoryRecord{
                .id = id,
                .key = std::move(key),
                .content = std::move(content),
                .kind = magic_enum::enum_cast<memory::memory_type>(kind, magic_enum::case_insensitive).value_or(memory::memory_type::user),
                .scope = std::move(scope),
                .updated_at = std::move(updated_at),
            };
        }
    };

} // namespace orangutan::sqlite

namespace orangutan::memory {
    namespace {

        inline constexpr std::size_t DEFAULT_SEARCH_LIMIT = 8;
        inline constexpr std::size_t DEFAULT_LIST_LIMIT = 20;
        inline constexpr std::size_t SEARCH_MATCH_LIMIT = 200;
        inline constexpr std::size_t SEARCH_SCAN_LIMIT = 200;

        [[nodiscard]]
        sqlite::Database open_memory_database(const std::filesystem::path &db_path) {
            auto db = sqlite::Database::create(db_path);
            if (!db) {
                throw std::runtime_error(db.error().to_string());
            }
            return std::move(*db);
        }

        void throw_if_failed(sqlite::SqliteResult<void> result) {
            if (!result) {
                throw std::runtime_error(result.error().to_string());
            }
        }

        [[nodiscard]]
        std::vector<std::string> tokenize_ascii_words(std::string_view value) {
            std::vector<std::string> tokens;
            std::string current;
            const auto flush = [&] {
                if (current.size() >= 2) {
                    tokens.push_back(utils::ascii_to_lower_copy(current));
                }
                current.clear();
            };

            for (const unsigned char ch : value) {
                if (std::isalnum(ch) != 0) {
                    current.push_back(static_cast<char>(ch));
                    continue;
                }
                flush();
            }
            flush();

            std::ranges::sort(tokens);
            tokens.erase(std::ranges::unique(tokens).begin(), tokens.end());
            return tokens;
        }

        [[nodiscard]]
        double substring_score(std::string_view haystack, std::string_view needle, double score) {
            return haystack.contains(needle) ? score : 0.0;
        }

        [[nodiscard]]
        double exact_score(std::string_view left, std::string_view right, double score) {
            return left == right ? score : 0.0;
        }

        [[nodiscard]]
        double token_score(const std::vector<std::string> &tokens, std::string_view haystack, double per_token_score) {
            double score = 0.0;
            for (const auto &token : tokens) {
                score += substring_score(haystack, token, per_token_score);
            }
            return score;
        }

        [[nodiscard]]
        double score_match(const MemoryRecord &record, std::string_view query) {
            const auto trimmed = utils::trim_copy(query);
            if (trimmed.empty()) {
                return 0.0;
            }

            const auto normalized_query = una::cases::to_lowercase_utf8(trimmed);
            const auto normalized_key = una::cases::to_lowercase_utf8(record.key);
            const auto normalized_content = una::cases::to_lowercase_utf8(record.content);
            const auto normalized_kind = una::cases::to_lowercase_utf8(std::string{magic_enum::enum_name(record.kind)});
            const auto tokens = tokenize_ascii_words(trimmed);

            double score = 0.0;
            score += exact_score(normalized_key, normalized_query, 100.0);
            score += exact_score(normalized_kind, normalized_query, 30.0);
            score += substring_score(normalized_key, normalized_query, 40.0);
            score += substring_score(normalized_content, normalized_query, 24.0);
            score += substring_score(normalized_kind, normalized_query, 8.0);
            score += token_score(tokens, normalized_key, 12.0);
            score += token_score(tokens, normalized_content, 6.0);
            score += token_score(tokens, normalized_kind, 4.0);
            return score;
        }

        [[nodiscard]]
        std::string format_records(const std::vector<MemoryRecord> &records) {
            std::string out;
            for (const auto &record : records) {
                utils::format_to(out, "[{}:{}] {}\n", magic_enum::enum_name(record.kind), record.key, record.content);
            }
            return out;
        }

    } // namespace

    MemoryStore::MemoryStore()
    : MemoryStore(memory_detail::default_db_path()) {}

    MemoryStore::MemoryStore(const std::filesystem::path &db_path)
    : db_(open_memory_database(db_path)) {
        ensure_schema();
    }

    MemoryStore::~MemoryStore() = default;

    void MemoryStore::ensure_schema() {
        memory_detail::create_current_schema(db_);
    }

    void MemoryStore::remember(std::string_view key, std::string_view content, memory_type kind, std::string_view scope) {
        const auto trimmed_key = utils::trim_copy(key);
        const auto trimmed_content = utils::trim_copy(content);
        if (trimmed_key.empty() || trimmed_content.empty()) {
            return;
        }

        std::scoped_lock lock(mutex_);
        const auto kind_name = std::string(magic_enum::enum_name(kind));
        auto command = db_.exec("INSERT INTO memories (scope, memory_key, content, kind, updated_at) "
                                "VALUES (?, ?, ?, ?, datetime('now')) "
                                "ON CONFLICT(scope, memory_key) DO UPDATE SET "
                                "content = excluded.content, kind = excluded.kind, updated_at = datetime('now')");
        if (!command) {
            throw std::runtime_error(command.error().to_string());
        }
        throw_if_failed(command->bind(scope, trimmed_key, trimmed_content, kind_name).run());
    }

    std::vector<MemoryRecord> MemoryStore::search(std::string_view query, std::string_view scope, std::size_t limit) {
        const auto trimmed_query = utils::trim_copy(query);
        if (trimmed_query.empty()) {
            return list(scope, limit == 0 ? DEFAULT_SEARCH_LIMIT : limit);
        }

        const auto effective_limit = limit == 0 ? DEFAULT_SEARCH_LIMIT : limit;
        std::scoped_lock lock(mutex_);

        std::unordered_map<int, MemoryRecord> candidates;
        const auto collect = [&candidates](std::vector<MemoryRecord> records) {
            for (auto &record : records) {
                candidates.insert_or_assign(record.id, std::move(record));
            }
        };

        const auto load_matches = [&](std::string_view needle) {
            if (needle.empty()) {
                return;
            }
            auto match_query = db_.query("SELECT id, memory_key, content, kind, scope, updated_at "
                                         "FROM memories WHERE scope = ? "
                                         "AND (memory_key = ? OR memory_key LIKE '%' || ? || '%' OR content LIKE '%' || ? || '%' OR kind LIKE '%' || ? || '%') "
                                         "ORDER BY updated_at DESC, id DESC LIMIT ?");
            if (!match_query) {
                throw std::runtime_error(match_query.error().to_string());
            }
            auto records = match_query->bind(scope, needle, needle, needle, needle, static_cast<int>(SEARCH_MATCH_LIMIT)).all<MemoryRecord>();
            if (!records) {
                throw std::runtime_error(records.error().to_string());
            }
            collect(std::move(*records));
        };

        load_matches(trimmed_query);
        for (const auto &token : tokenize_ascii_words(trimmed_query)) {
            load_matches(token);
        }

        auto recent_query = db_.query("SELECT id, memory_key, content, kind, scope, updated_at "
                                      "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
        if (!recent_query) {
            throw std::runtime_error(recent_query.error().to_string());
        }
        auto recent_records = recent_query->bind(scope, static_cast<int>(SEARCH_SCAN_LIMIT)).all<MemoryRecord>();
        if (!recent_records) {
            throw std::runtime_error(recent_records.error().to_string());
        }
        collect(std::move(*recent_records));

        struct RankedMemory {
            MemoryRecord record;
            double score = 0.0;
        };

        std::vector<RankedMemory> ranked;
        ranked.reserve(candidates.size());
        for (auto &[id, record] : candidates) {
            static_cast<void>(id);
            const auto score = score_match(record, trimmed_query);
            if (score > 0.0) {
                ranked.push_back({.record = std::move(record), .score = score});
            }
        }

        std::ranges::sort(ranked, [](const RankedMemory &left, const RankedMemory &right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.record.updated_at != right.record.updated_at) {
                return left.record.updated_at > right.record.updated_at;
            }
            return left.record.id > right.record.id;
        });

        std::vector<MemoryRecord> selected;
        selected.reserve(std::min(effective_limit, ranked.size()));
        for (auto &entry : ranked) {
            if (selected.size() >= effective_limit) {
                break;
            }
            selected.push_back(std::move(entry.record));
        }
        return selected;
    }

    std::string MemoryStore::recall(std::string_view query, std::string_view scope, std::size_t limit) {
        return format_records(search(query, scope, limit));
    }

    std::vector<MemoryRecord> MemoryStore::list(std::string_view scope, std::size_t limit) {
        std::scoped_lock lock(mutex_);
        const auto effective_limit = static_cast<int>(limit == 0 ? DEFAULT_LIST_LIMIT : limit);
        auto query = db_.query("SELECT id, memory_key, content, kind, scope, updated_at "
                               "FROM memories WHERE scope = ? ORDER BY updated_at DESC, id DESC LIMIT ?");
        if (!query) {
            throw std::runtime_error(query.error().to_string());
        }
        auto records = query->bind(scope, effective_limit).all<MemoryRecord>();
        if (!records) {
            throw std::runtime_error(records.error().to_string());
        }
        return std::move(*records);
    }

    bool MemoryStore::forget(std::string_view key, std::string_view scope) {
        std::scoped_lock lock(mutex_);
        auto command = db_.exec("DELETE FROM memories WHERE scope = ? AND memory_key = ?");
        if (!command) {
            throw std::runtime_error(command.error().to_string());
        }
        throw_if_failed(command->bind(scope, key).run());
        return db_.changes() > 0;
    }

} // namespace orangutan::memory
