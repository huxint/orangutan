// src/oran-memory/longterm.cpp — long-term memory contract validation.

#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/str.hpp>

#include "_impl/text.hpp"

namespace orangutan::memory::longterm {
namespace {

[[nodiscard]] bool contains_control_char(std::string_view value, bool allow_multiline) noexcept {
  return std::ranges::any_of(value, [allow_multiline](char ch) {
    if (allow_multiline && (ch == '\t' || ch == '\n' || ch == '\r')) {
      return false;
    }
    return static_cast<unsigned char>(ch) < 0x20U;
  });
}

[[nodiscard]] core::Error invalid_field(std::string field, std::string message) {
  return core::Error::invalid_argument(std::move(message)).with("field", std::move(field));
}

[[nodiscard]] core::Result<void>
validate_required(std::string_view value, std::string field, bool allow_multiline = false) {
  if (std::ranges::all_of(value, detail::is_space)) {
    return std::unexpected(invalid_field(std::move(field), "long-term memory field must not be blank"));
  }
  if (!core::str::is_valid_utf8(value) || contains_control_char(value, allow_multiline)) {
    return std::unexpected(
        invalid_field(std::move(field), "long-term memory field must be UTF-8 without control characters"));
  }
  return {};
}

template <typename T>
[[nodiscard]] bool has_duplicates(std::vector<T> values) {
  std::ranges::sort(values);
  return std::ranges::adjacent_find(values) != values.end();
}

[[nodiscard]] core::Result<void> validate_kinds(const std::vector<RecordKind>& kinds) {
  if (has_duplicates(kinds)) {
    return std::unexpected(invalid_field("kinds", "long-term memory query kind filters must be unique"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_string_list(const std::vector<std::string>& values, std::string field) {
  for (const auto& value : values) {
    if (auto valid = validate_required(value, field); !valid) {
      return valid;
    }
  }
  if (has_duplicates(values)) {
    return std::unexpected(invalid_field(std::move(field), "long-term memory string list values must be unique"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_limit(std::size_t limit) {
  if (limit == 0 || limit > 100) {
    return std::unexpected(invalid_field("limit", "long-term memory search limit must be between 1 and 100"));
  }
  return {};
}

}  // namespace

core::Result<void> validate_key(const RecordKey& key) {
  if (auto valid = validate_required(key.id, "record_id"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(key.scope_key, "scope_key"); !valid) {
    return valid;
  }
  return {};
}

core::Result<void> validate_record(const Record& record) {
  if (auto valid = validate_key(record.key); !valid) {
    return valid;
  }
  if (auto valid = validate_required(record.title, "title"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(record.body, "body", true); !valid) {
    return valid;
  }
  if (!std::isfinite(record.importance) || record.importance < 0.0 || record.importance > 1.0) {
    return std::unexpected(
        invalid_field("importance", "long-term memory importance must be a finite value in the range [0, 1]"));
  }
  if (record.updated_at < record.created_at) {
    return std::unexpected(
        invalid_field("updated_at", "long-term memory updated_at must not be earlier than created_at"));
  }
  if (record.last_read_at < record.created_at) {
    return std::unexpected(
        invalid_field("last_read_at", "long-term memory last_read_at must not be earlier than created_at"));
  }
  if (auto valid = validate_string_list(record.tags, "tag"); !valid) {
    return valid;
  }
  if (auto valid = validate_string_list(record.linked_record_ids, "linked_record_id"); !valid) {
    return valid;
  }
  return {};
}

core::Result<void> validate_query(const Query& query, std::size_t limit) {
  if (auto valid = validate_required(query.scope_key, "scope_key"); !valid) {
    return valid;
  }
  if (auto valid = validate_required(query.text, "query_text", true); !valid) {
    return valid;
  }
  if (query.text.size() > 4096) {
    return std::unexpected(invalid_field("query_text", "long-term memory query must not exceed 4096 bytes"));
  }
  if (auto valid = validate_kinds(query.kinds); !valid) {
    return valid;
  }
  return validate_limit(limit);
}

core::Result<void> validate_recall_request(const RecallRequest& request) {
  if (request.record_id.empty()) {
    return validate_query(request.query, request.limit);
  }
  if (!request.query.text.empty()) {
    return std::unexpected(invalid_field("record_id", "memory recall accepts an ID or search text, not both"));
  }
  if (auto valid = validate_key(RecordKey{.id = request.record_id, .scope_key = request.query.scope_key}); !valid) {
    return valid;
  }
  if (auto valid = validate_kinds(request.query.kinds); !valid) {
    return valid;
  }
  return validate_limit(request.limit);
}

core::Result<void> validate_index_request(const IndexRequest& request) {
  if (auto valid = validate_required(request.scope_key, "scope_key"); !valid) {
    return valid;
  }
  if (auto valid = validate_kinds(request.kinds); !valid) {
    return valid;
  }
  if (request.limit == 0 || request.limit > 20) {
    return std::unexpected(invalid_field("limit", "memory index limit must be between 1 and 20"));
  }
  if (request.offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) - 100) {
    return std::unexpected(invalid_field("offset", "memory index offset is too large"));
  }
  if (request.max_bytes < 512 || request.max_bytes > kMaxIndexBytes) {
    return std::unexpected(invalid_field("max_bytes", "memory index budget must be between 512 and 8192 bytes"));
  }
  return {};
}

}  // namespace orangutan::memory::longterm
