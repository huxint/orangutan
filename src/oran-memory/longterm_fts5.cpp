// src/oran-memory/longterm_fts5.cpp — SQLite FTS5 long-term memory backend.

#include <oran/memory/longterm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::memory::longterm {
namespace {

using json = nlohmann::ordered_json;

constexpr unsigned char kLongtermFts5InitialBytes[] = {
#embed "migrations/longterm/0001-longterm-fts5-initial.sql"
};

constexpr std::string_view kGetRecordSql = R"sql(
SELECT scope_key,
       id,
       kind,
       title,
       body,
       created_at,
       updated_at,
       last_read_at,
       importance,
       tags_json,
       linked_record_ids_json,
       shadow
FROM longterm_records
WHERE scope_key = ? AND id = ?
)sql";

constexpr std::string_view kUpsertRecordSql = R"sql(
INSERT INTO longterm_records(
  scope_key,
  id,
  kind,
  title,
  body,
  created_at,
  updated_at,
  last_read_at,
  importance,
  tags_json,
  linked_record_ids_json,
  shadow
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(scope_key, id) DO UPDATE SET
  kind = excluded.kind,
  title = excluded.title,
  body = excluded.body,
  created_at = longterm_records.created_at,
  updated_at = MAX(longterm_records.updated_at, excluded.updated_at),
  last_read_at = longterm_records.last_read_at,
  importance = excluded.importance,
  tags_json = excluded.tags_json,
  linked_record_ids_json = excluded.linked_record_ids_json,
  shadow = excluded.shadow
RETURNING scope_key,
          id,
          kind,
          title,
          body,
          created_at,
          updated_at,
          last_read_at,
          importance,
          tags_json,
          linked_record_ids_json,
          shadow
)sql";

constexpr std::string_view kDeleteFtsSql = R"sql(
DELETE FROM longterm_records_fts
WHERE scope_key = ? AND record_id = ?
)sql";

constexpr std::string_view kInsertFtsSql = R"sql(
INSERT INTO longterm_records_fts(scope_key, record_id, kind, shadow, title, body, tags)
VALUES (?, ?, ?, ?, ?, ?, ?)
)sql";

constexpr std::string_view kDeleteRecordSql = R"sql(
DELETE FROM longterm_records
WHERE scope_key = ? AND id = ?
)sql";

constexpr std::string_view kTouchRecordSql = R"sql(
UPDATE longterm_records
SET last_read_at = ?
WHERE scope_key = ? AND id = ? AND last_read_at < ?
)sql";

constexpr std::string_view kSearchSelectSql = R"sql(
SELECT r.scope_key,
       r.id,
       r.kind,
       r.title,
       r.body,
       r.created_at,
       r.updated_at,
       r.last_read_at,
       r.importance,
       r.tags_json,
       r.linked_record_ids_json,
       r.shadow,
       -bm25(longterm_records_fts, 0, 0, 0, 0, 4, 1, 2) AS lexical_score
FROM longterm_records_fts
JOIN longterm_records AS r
  ON r.scope_key = longterm_records_fts.scope_key
 AND r.id = longterm_records_fts.record_id
WHERE longterm_records_fts MATCH ?
  AND longterm_records_fts.scope_key = ?
  AND r.shadow = 0
)sql";

constexpr std::string_view kSearchOrderSql = R"sql(
ORDER BY lexical_score DESC, r.updated_at DESC, r.id ASC
LIMIT ?
)sql";

template <std::size_t N>
[[nodiscard]] std::string to_sql_string(const unsigned char (&bytes)[N]) {
  return std::string{reinterpret_cast<const char*>(bytes), N};
}

[[nodiscard]] std::span<const storage::Migration> built_in_longterm_migrations() {
  static const std::array<storage::Migration, 1> kMigrations{
      storage::Migration{
          .version = 1,
          .name = "longterm-fts5-initial",
          .sql = to_sql_string(kLongtermFts5InitialBytes),
      },
  };
  return kMigrations;
}

[[nodiscard]] core::Result<std::vector<std::string>> string_list_from_json(std::string_view text,
                                                                           std::string_view field) {
  json parsed;
  try {
    parsed = json::parse(text);
  } catch (const json::parse_error& error) {
    return std::unexpected(core::Error::storage("long-term memory string-list JSON is malformed")
                               .with("field", std::string{field})
                               .with("detail", error.what()));
  }
  if (!parsed.is_array()) {
    return std::unexpected(
        core::Error::storage("long-term memory string-list JSON is not an array").with("field", std::string{field}));
  }

  std::vector<std::string> out;
  out.reserve(parsed.size());
  for (std::size_t i = 0; i < parsed.size(); ++i) {
    if (!parsed[i].is_string()) {
      return std::unexpected(core::Error::storage("long-term memory string-list JSON has a non-string value")
                                 .with("field", std::string{field})
                                 .with("index", std::to_string(i)));
    }
    out.push_back(parsed[i].get<std::string>());
  }
  return out;
}

[[nodiscard]] std::string string_list_to_json(const std::vector<std::string>& values) {
  auto out = json::array();
  for (const auto& value : values) {
    out.push_back(value);
  }
  return out.dump();
}

[[nodiscard]] std::string joined_tags(std::span<const std::string> tags) {
  std::string out;
  for (const auto& tag : tags) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += tag;
  }
  return out;
}

[[nodiscard]] core::Result<RecordKind> record_kind_from_text(std::string text) {
  auto kind = core::parse_enum<RecordKind>(text);
  if (!kind) {
    return std::unexpected(core::Error::storage("long-term memory row has unknown kind").with("kind", std::move(text)));
  }
  return *kind;
}

[[nodiscard]] core::Result<Record> read_record_row(storage::Statement& statement, int offset = 0) {
  auto scope_key = statement.required_text(offset + 0, "scope_key");
  if (!scope_key) {
    return std::unexpected(std::move(scope_key).error());
  }
  auto id = statement.required_text(offset + 1, "id");
  if (!id) {
    return std::unexpected(std::move(id).error());
  }
  auto kind_text = statement.required_text(offset + 2, "kind");
  if (!kind_text) {
    return std::unexpected(std::move(kind_text).error());
  }
  auto kind = record_kind_from_text(std::move(*kind_text));
  if (!kind) {
    return std::unexpected(std::move(kind).error());
  }
  auto title = statement.required_text(offset + 3, "title");
  if (!title) {
    return std::unexpected(std::move(title).error());
  }
  auto body = statement.required_text(offset + 4, "body");
  if (!body) {
    return std::unexpected(std::move(body).error());
  }
  auto created_at_text = statement.required_text(offset + 5, "created_at");
  if (!created_at_text) {
    return std::unexpected(std::move(created_at_text).error());
  }
  auto created_at = core::time::parse_iso8601_utc(*created_at_text);
  if (!created_at) {
    return std::unexpected(std::move(created_at).error().with("field", "created_at"));
  }
  auto updated_at_text = statement.required_text(offset + 6, "updated_at");
  if (!updated_at_text) {
    return std::unexpected(std::move(updated_at_text).error());
  }
  auto updated_at = core::time::parse_iso8601_utc(*updated_at_text);
  if (!updated_at) {
    return std::unexpected(std::move(updated_at).error().with("field", "updated_at"));
  }
  auto last_read_at_text = statement.required_text(offset + 7, "last_read_at");
  if (!last_read_at_text) {
    return std::unexpected(std::move(last_read_at_text).error());
  }
  auto last_read_at = core::time::parse_iso8601_utc(*last_read_at_text);
  if (!last_read_at) {
    return std::unexpected(std::move(last_read_at).error().with("field", "last_read_at"));
  }
  auto importance = statement.column_double(offset + 8);
  if (!importance) {
    return std::unexpected(std::move(importance).error().with("field", "importance"));
  }
  auto tags_json = statement.required_text(offset + 9, "tags_json");
  if (!tags_json) {
    return std::unexpected(std::move(tags_json).error());
  }
  auto tags = string_list_from_json(*tags_json, "tags_json");
  if (!tags) {
    return std::unexpected(std::move(tags).error());
  }
  auto linked_json = statement.required_text(offset + 10, "linked_record_ids_json");
  if (!linked_json) {
    return std::unexpected(std::move(linked_json).error());
  }
  auto linked_ids = string_list_from_json(*linked_json, "linked_record_ids_json");
  if (!linked_ids) {
    return std::unexpected(std::move(linked_ids).error());
  }
  auto shadow = statement.column_int64(offset + 11);
  if (!shadow) {
    return std::unexpected(std::move(shadow).error().with("field", "shadow"));
  }
  if (*shadow != 0 && *shadow != 1) {
    return std::unexpected(
        core::Error::storage("long-term memory row has invalid shadow value").with("shadow", std::to_string(*shadow)));
  }

  return Record{
      .key = RecordKey{.id = std::move(*id), .scope_key = std::move(*scope_key)},
      .kind = *kind,
      .title = std::move(*title),
      .body = std::move(*body),
      .created_at = *created_at,
      .updated_at = *updated_at,
      .last_read_at = *last_read_at,
      .importance = *importance,
      .tags = std::move(*tags),
      .linked_record_ids = std::move(*linked_ids),
      .shadow = *shadow == 1,
  };
}

[[nodiscard]] core::Result<void>
delete_fts_row(storage::Connection& connection, storage::StatementCache& cache, const RecordKey& key) {
  auto cached = cache.acquire(connection, kDeleteFtsSql);
  if (!cached) {
    return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(key.scope_key, key.id); !bound) {
    return bound;
  }
  return statement.expect_done("delete_fts");
}

[[nodiscard]] std::string quote_fts_token(std::string_view token) {
  std::string out;
  out.reserve(token.size() + 2);
  out.push_back('"');
  for (const auto ch : token) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

[[nodiscard]] std::string make_fts_query(std::string_view text) {
  const auto word_byte = [](char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return byte >= 0x80 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
  };
  std::string out;
  std::size_t position = 0;
  std::size_t terms = 0;
  while (position < text.size() && terms < 32) {
    while (position < text.size() && !word_byte(text[position])) {
      ++position;
    }
    const auto start = position;
    while (position < text.size() && word_byte(text[position])) {
      ++position;
    }
    if (position > start) {
      if (!out.empty()) {
        out += " OR ";
      }
      out += quote_fts_token(text.substr(start, position - start));
      ++terms;
    }
  }
  return out;
}

[[nodiscard]] std::string search_sql_for(const Query& query) {
  std::string sql{kSearchSelectSql};
  if (!query.kinds.empty()) {
    sql += "  AND r.kind IN (";
    for (std::size_t i = 0; i < query.kinds.size(); ++i) {
      if (i != 0) {
        sql += ", ";
      }
      sql.push_back('?');
    }
    sql += ")\n";
  }
  sql += kSearchOrderSql;
  return sql;
}

}  // namespace

Fts5Backend::Fts5Backend(storage::Pool& pool, Fts5BackendOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<storage::MigrationReport>> Fts5Backend::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = storage::run_migrations(writer->connection(), built_in_longterm_migrations());
    if (!report) {
      co_return std::unexpected(std::move(report).error());
    }
    co_return std::move(*report);
  }

  auto report = storage::run_migrations_from_directory(writer->connection(), options_.migrations_directory);
  if (!report) {
    co_return std::unexpected(std::move(report).error());
  }
  co_return std::move(*report);
}

async::Awaitable<core::Result<Record>> Fts5Backend::get(RecordKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }
  auto cached = reader->statement_cache().acquire(reader->connection(), kGetRecordSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(key.scope_key, key.id); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(std::move(step).error());
  }
  if (*step == storage::StepResult::done) {
    co_return std::unexpected(core::Error::not_found("long-term memory record not found")
                                  .with("scope_key", key.scope_key)
                                  .with("id", key.id));
  }

  auto record = read_record_row(statement);
  if (!record) {
    co_return std::unexpected(std::move(record).error());
  }
  if (auto done = statement.expect_done("get_record"); !done) {
    co_return std::unexpected(std::move(done).error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::vector<SearchHit>>> Fts5Backend::search(Query query, std::size_t limit) {
  if (auto valid = validate_query(query, limit); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  const auto match_query = make_fts_query(query.text);
  if (match_query.empty()) {
    co_return std::vector<SearchHit>{};
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }

  const auto sql = search_sql_for(query);
  auto cached = reader->statement_cache().acquire(reader->connection(), sql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  int bind_index = 1;
  if (auto bound = statement.bind_text(bind_index++, match_query); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto bound = statement.bind_text(bind_index++, query.scope_key); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  for (const auto kind : query.kinds) {
    if (auto bound = statement.bind_text(bind_index, core::enum_name(kind)); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    ++bind_index;
  }
  if (auto bound = statement.bind_int64(bind_index, static_cast<std::int64_t>(limit)); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }

  std::vector<SearchHit> hits;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto record = read_record_row(statement, 0);
    if (!record) {
      co_return std::unexpected(std::move(record).error());
    }
    auto lexical = statement.column_double(12);
    if (!lexical) {
      co_return std::unexpected(std::move(lexical).error().with("field", "lexical_score"));
    }
    hits.push_back(SearchHit{
        .record = std::move(*record),
        .score = *lexical,
    });
  }
  co_return hits;
}

async::Awaitable<core::Result<Record>> Fts5Backend::upsert(Record record) {
  if (auto valid = validate_record(record); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& connection = writer->connection();
  auto& cache = writer->statement_cache();

  auto transaction = storage::Transaction::begin(connection);
  if (!transaction) {
    co_return std::unexpected(std::move(transaction).error());
  }

  const auto tags_json = string_list_to_json(record.tags);
  const auto linked_json = string_list_to_json(record.linked_record_ids);
  auto stored = Record{};
  {
    auto cached = cache.acquire(connection, kUpsertRecordSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& statement = cached->statement();
    if (auto bound = statement.bind_all(record.key.scope_key,
                                            record.key.id,
                                            core::enum_name(record.kind),
                                            record.title,
                                            record.body,
                                            core::time::format_iso8601_utc(record.created_at),
                                            core::time::format_iso8601_utc(record.updated_at),
                                            core::time::format_iso8601_utc(record.last_read_at),
                                            record.importance,
                                            tags_json,
                                            linked_json,
                                            record.shadow); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step != storage::StepResult::row) {
      co_return std::unexpected(
          core::Error::storage("long-term memory upsert returned no row"));
    }
    auto row = read_record_row(statement);
    if (!row) {
      co_return std::unexpected(std::move(row).error());
    }
    stored = std::move(*row);
    if (auto done = statement.expect_done("upsert_record"); !done) {
      co_return std::unexpected(std::move(done).error());
    }
  }

  if (auto deleted = delete_fts_row(connection, cache, stored.key); !deleted) {
    co_return std::unexpected(std::move(deleted).error());
  }
  {
    auto cached = cache.acquire(connection, kInsertFtsSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& statement = cached->statement();
    if (auto bound = statement.bind_all(stored.key.scope_key,
                                        stored.key.id,
                                        core::enum_name(stored.kind),
                                        stored.shadow,
                                        stored.title,
                                        stored.body,
                                        joined_tags(stored.tags));
        !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    if (auto inserted = statement.expect_done("insert_fts"); !inserted) {
      co_return std::unexpected(std::move(inserted).error());
    }
  }

  if (auto committed = transaction->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return stored;
}

async::Awaitable<core::Result<void>> Fts5Backend::touch(std::vector<RecordKey> keys, core::Time read_at) {
  for (const auto& key : keys) {
    if (auto valid = validate_key(key); !valid) {
      co_return std::unexpected(std::move(valid).error());
    }
  }
  if (keys.empty()) {
    co_return core::Result<void>{};
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& connection = writer->connection();
  auto transaction = storage::Transaction::begin(connection);
  if (!transaction) {
    co_return std::unexpected(std::move(transaction).error());
  }
  auto cached = writer->statement_cache().acquire(connection, kTouchRecordSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& statement = cached->statement();
  const auto read_at_text = core::time::format_iso8601_utc(read_at);
  for (const auto& key : keys) {
    if (auto reset = statement.reset(); !reset) {
      co_return std::unexpected(std::move(reset).error());
    }
    if (auto bound = statement.bind_all(read_at_text, key.scope_key, key.id, read_at_text); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    if (auto done = statement.expect_done("touch_record"); !done) {
      co_return std::unexpected(std::move(done).error());
    }
  }
  if (auto committed = transaction->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> Fts5Backend::remove(RecordKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& connection = writer->connection();
  auto& cache = writer->statement_cache();

  auto transaction = storage::Transaction::begin(connection);
  if (!transaction) {
    co_return std::unexpected(std::move(transaction).error());
  }
  if (auto deleted = delete_fts_row(connection, cache, key); !deleted) {
    co_return std::unexpected(std::move(deleted).error());
  }
  {
    auto cached = cache.acquire(connection, kDeleteRecordSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& statement = cached->statement();
    if (auto bound = statement.bind_all(key.scope_key, key.id); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    if (auto deleted = statement.expect_done("delete_record"); !deleted) {
      co_return std::unexpected(std::move(deleted).error());
    }
  }
  if (auto committed = transaction->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return core::Result<void>{};
}

}  // namespace orangutan::memory::longterm
