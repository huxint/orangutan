#include <oran/memory/longterm.hpp>

#include <expected>
#include <string>
#include <utility>

#include <asio/this_coro.hpp>

#include <oran/core/error.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::memory::longterm {

async::Awaitable<core::Result<void>> Fts5Backend::backup_to(std::string destination) {
  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }
  co_return reader->connection().backup_to(destination);
}

async::Awaitable<core::Result<void>>
Fts5Backend::import_scope(std::string source_path, std::string source_scope, std::string destination_scope) {
  if (source_path.empty() || source_path.contains('\0')) {
    co_return std::unexpected(core::Error::invalid_argument("memory import requires a source snapshot path"));
  }
  for (const auto& scope : {source_scope, destination_scope}) {
    if (auto valid = validate_index_request(IndexRequest{.scope_key = scope}); !valid) {
      co_return std::unexpected(std::move(valid).error());
    }
  }
  if (auto cancellation = co_await asio::this_coro::cancellation_state;
      cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }
  auto input = storage::Connection::open(
      {.path = std::move(source_path), .mode = storage::OpenMode::read_only, .enable_wal = false});
  if (!input) {
    co_return std::unexpected(std::move(input).error());
  }
  // A read transaction pins the source schema and records to one snapshot.
  if (auto begun = input->execute("BEGIN"); !begun) {
    co_return std::unexpected(std::move(begun).error());
  }
  auto version = input->query("SELECT MAX(version) FROM schema_versions");
  if (!version) {
    co_return std::unexpected(std::move(version).error());
  }
  if (version->rows.size() != 1 || version->rows[0].values[0] != "1") {
    co_return std::unexpected(core::Error::invalid_argument("unsupported memory import schema"));
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto& output = writer->connection();
  auto transaction = storage::Transaction::begin(output);
  if (!transaction) {
    co_return std::unexpected(std::move(transaction).error());
  }
  auto existing = output.prepare(R"sql(
SELECT 1 FROM longterm_records WHERE scope_key=?1
UNION ALL SELECT 1 FROM longterm_records_fts WHERE scope_key=?1 LIMIT 1
)sql");
  if (!existing) {
    co_return std::unexpected(std::move(existing).error());
  }
  if (auto bound = existing->bind_all(destination_scope); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  auto found = existing->step();
  if (!found) {
    co_return std::unexpected(std::move(found).error());
  }
  if (*found != storage::StepResult::done) {
    co_return std::unexpected(core::Error{core::ErrorKind::conflict, "memory import destination scope already exists"});
  }

  auto read = input->prepare(R"sql(
SELECT id,kind,title,body,created_at,updated_at,last_read_at,tags_json,linked_record_ids_json,importance,shadow
FROM longterm_records WHERE scope_key=? ORDER BY id
)sql");
  if (!read) {
    co_return std::unexpected(std::move(read).error());
  }
  if (auto bound = read->bind_all(source_scope); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  auto write = output.prepare(R"sql(
INSERT INTO longterm_records(scope_key,id,kind,title,body,created_at,updated_at,last_read_at,
                            tags_json,linked_record_ids_json,importance,shadow)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
)sql");
  if (!write) {
    co_return std::unexpected(std::move(write).error());
  }
  auto index = output.prepare(R"sql(
INSERT INTO longterm_records_fts(scope_key,record_id,kind,shadow,title,body,tags)
SELECT scope_key,id,kind,shadow,title,body,
       COALESCE((SELECT group_concat(value,' ') FROM json_each(tags_json)), '')
FROM longterm_records WHERE scope_key=? AND id=?
)sql");
  if (!index) {
    co_return std::unexpected(std::move(index).error());
  }

  bool copied = false;
  for (;;) {
    if (auto cancellation = co_await asio::this_coro::cancellation_state;
        cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    auto step = read->step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    if (auto bound = write->bind_text(1, destination_scope); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    for (int column = 0; column < 9; ++column) {
      auto value = read->column_text(column);
      if (!value) {
        co_return std::unexpected(std::move(value).error());
      }
      const auto bound = *value ? write->bind_text(column + 2, **value) : write->bind_null(column + 2);
      if (!bound) {
        co_return std::unexpected(bound.error());
      }
    }
    auto importance = read->column_double(9);
    if (!importance) {
      co_return std::unexpected(std::move(importance).error());
    }
    if (auto bound = write->bind_double(11, *importance); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    auto shadow = read->column_int64(10);
    if (!shadow) {
      co_return std::unexpected(std::move(shadow).error());
    }
    if (auto bound = write->bind_int64(12, *shadow); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    if (auto done = write->expect_done("import memory record"); !done) {
      co_return std::unexpected(std::move(done).error());
    }
    if (auto reset = write->reset(); !reset) {
      co_return std::unexpected(std::move(reset).error());
    }
    auto id = read->required_text(0, "id");
    if (!id) {
      co_return std::unexpected(std::move(id).error());
    }
    if (auto bound = index->bind_all(destination_scope, *id); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    if (auto done = index->expect_done("index imported memory record"); !done) {
      co_return std::unexpected(std::move(done).error());
    }
    if (auto reset = index->reset(); !reset) {
      co_return std::unexpected(std::move(reset).error());
    }
    copied = true;
  }
  if (!copied) {
    co_return std::unexpected(core::Error::not_found("memory import source scope not found"));
  }
  co_return transaction->commit();
}

}  // namespace orangutan::memory::longterm
