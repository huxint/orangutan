#pragma once

#include "features/memory/memory.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory_detail {

inline constexpr size_t default_search_limit = 8;
inline constexpr size_t default_list_limit = 20;
inline constexpr size_t search_scan_limit = 200;

[[nodiscard]]
std::string trim_copy(std::string value);
[[nodiscard]]
std::vector<std::string> split_memory_fragments(const std::string &value);
[[nodiscard]]
std::string normalize_ascii(std::string_view value);
[[nodiscard]]
std::vector<std::string> tokenize_ascii_words(std::string_view value);
[[nodiscard]]
bool contains_non_ascii(std::string_view value);
[[nodiscard]]
double score_memory_match(const MemoryRecord &record, const std::string &query);
[[nodiscard]]
std::string format_records(const std::vector<MemoryRecord> &records);
[[nodiscard]]
std::string merge_memory_content(const std::string &existing, const std::string &incoming);
[[nodiscard]]
std::optional<std::string> build_fts_query(const std::string &query);
[[nodiscard]]
std::vector<MemoryRecord> collect_records(sqlite::Statement &stmt);
[[nodiscard]]
std::optional<MemoryRecord> fetch_memory_by_key(sqlite::Database &db, const std::string &scope, const std::string &key);
void upsert_memory_record(sqlite::Database &db, const std::string &scope, const std::string &key, const std::string &content, const std::string &category,
                          const std::string &source, double importance);
[[nodiscard]]
std::vector<MemoryRecord> dedupe_records_by_key(std::vector<MemoryRecord> records);
void touch_records(sqlite::Database &db, const std::vector<MemoryRecord> &records);

} // namespace orangutan::memory_detail
