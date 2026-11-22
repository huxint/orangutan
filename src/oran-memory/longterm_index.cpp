#include <oran/memory/longterm.hpp>

#include <array>
#include <utility>

#include <oran/core/error.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::memory::longterm {
namespace {

[[nodiscard]] std::string index_sql(const IndexRequest& request) {
  // Bound strings in SQLite before copying them out. One extra code point lets
  // the pure renderer mark clipped cues. Oversized legacy IDs are never aliased.
  std::string sql = "SELECT substr(id, 1, 8193), kind, substr(title, 1, 121), substr(body, 1, 241) "
                    "FROM longterm_records WHERE scope_key = ? AND shadow = 0";
  if (!request.kinds.empty()) {
    sql += " AND kind IN (";
    for (std::size_t i = 0; i < request.kinds.size(); ++i) {
      sql += i == 0 ? "?" : ", ?";
    }
    sql += ")";
  }
  sql += " ORDER BY CASE kind WHEN 'feedback' THEN 0 WHEN 'user' THEN 1 ELSE 2 END, "
         "importance DESC, updated_at DESC, id ASC LIMIT ? OFFSET ?";
  return sql;
}

[[nodiscard]] core::Result<IndexEntry> read_entry(storage::Statement& statement, std::string scope_key) {
  std::array<std::string, 4> values;
  for (std::size_t i = 0; i < values.size(); ++i) {
    auto value = statement.column_text(static_cast<int>(i));
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    if (!value->has_value()) {
      return std::unexpected(core::Error::storage("memory index has a null required field"));
    }
    values[i] = std::move(**value);
  }
  auto kind = core::parse_enum<RecordKind>(values[1]);
  if (!kind) {
    return std::unexpected(core::Error::storage("memory index has an unknown record kind"));
  }
  return IndexEntry{
      .key = RecordKey{.id = std::move(values[0]), .scope_key = std::move(scope_key)},
      .kind = *kind,
      .title = std::move(values[2]),
      .summary = std::move(values[3]),
  };
}

}  // namespace

async::Awaitable<core::Result<std::vector<IndexEntry>>> Fts5Backend::list(IndexRequest request) {
  if (auto valid = validate_index_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }
  auto cached = reader->statement_cache().acquire(reader->connection(), index_sql(request));
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_text(1, request.scope_key); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  int parameter = 2;
  for (const auto kind : request.kinds) {
    if (auto bound = statement.bind_text(parameter++, core::enum_name(kind)); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
  }
  if (auto bound = statement.bind_int64(parameter++, static_cast<std::int64_t>(request.limit + 1)); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto bound = statement.bind_int64(parameter, static_cast<std::int64_t>(request.offset)); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }

  std::vector<IndexEntry> entries;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto entry = read_entry(statement, request.scope_key);
    if (!entry) {
      co_return std::unexpected(std::move(entry).error());
    }
    entries.push_back(std::move(*entry));
  }
  co_return entries;
}

}  // namespace orangutan::memory::longterm
