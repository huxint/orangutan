#pragma once

#include "types/types.hpp"
#include "memory/memory-store.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory::detail {

    inline constexpr std::size_t DEFAULT_SEARCH_LIMIT = 8;
    inline constexpr std::size_t DEFAULT_LIST_LIMIT = 20;
    inline constexpr std::size_t SEARCH_SCAN_LIMIT = 200;
    inline constexpr std::size_t SEARCH_MATCH_CANDIDATE_LIMIT = 64;

    [[nodiscard]]
    std::vector<std::string> split_memory_fragments(std::string_view value);
    [[nodiscard]]
    std::vector<std::string> tokenize_ascii_words(std::string_view value);
    [[nodiscard]]
    bool contains_non_ascii(std::string_view value);
    [[nodiscard]]
    double score_memory_match(const MemoryRecord &record, std::string_view query);
    [[nodiscard]]
    std::string format_records(const std::vector<MemoryRecord> &records);
    [[nodiscard]]
    std::string format_memory_manifest(const std::vector<MemoryRecord> &records);
    [[nodiscard]]
    std::string merge_memory_content(std::string_view existing, std::string_view incoming);
    [[nodiscard]]
    std::optional<std::string> build_fts_query(std::string_view query);
    [[nodiscard]]
    MemoryRecord read_memory_record(const sqlite::Row &row);
    [[nodiscard]]
    std::optional<MemoryRecord> fetch_memory_by_key(sqlite::Database &db, std::string_view scope, std::string_view key);
    void upsert_memory_record(sqlite::Database &db, std::string_view scope, std::string_view key, std::string_view content, std::string_view category, std::string_view type,
                              std::string_view source, double importance);
    void touch_records(sqlite::Database &db, const std::vector<MemoryRecord> &records);

} // namespace orangutan::memory::detail
