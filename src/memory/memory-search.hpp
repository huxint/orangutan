#pragma once

#include "types/types.hpp"
#include "memory/memory-store.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory::detail {

    inline constexpr std::size_t default_search_limit = 8;
    inline constexpr std::size_t default_list_limit = 20;
    inline constexpr std::size_t search_scan_limit = 200;

    [[nodiscard]]
    std::vector<std::string> split_memory_fragments(std::string_view value);
    [[nodiscard]]
    std::vector<std::string> tokenize_ascii_words(std::string_view value);
    [[nodiscard]]
    bool contains_non_ascii(std::string_view value);
    [[nodiscard]]
    base::f64 score_memory_match(const MemoryRecord &record, const std::string &query);
    [[nodiscard]]
    std::string format_records(const std::vector<MemoryRecord> &records);
    [[nodiscard]]
    std::string format_memory_manifest(const std::vector<MemoryRecord> &records);
    [[nodiscard]]
    std::string merge_memory_content(const std::string &existing, const std::string &incoming);
    [[nodiscard]]
    std::optional<std::string> build_fts_query(const std::string &query);
    [[nodiscard]]
    std::vector<MemoryRecord> collect_records(sqlite::Statement &stmt);
    [[nodiscard]]
    std::optional<MemoryRecord> fetch_memory_by_key(sqlite::Database &db, const std::string &scope, const std::string &key);
    void upsert_memory_record(sqlite::Database &db, const std::string &scope, const std::string &key, const std::string &content, const std::string &category,
                              const std::string &type, const std::string &source, base::f64 importance);
    void touch_records(sqlite::Database &db, const std::vector<MemoryRecord> &records);

} // namespace orangutan::memory::detail
