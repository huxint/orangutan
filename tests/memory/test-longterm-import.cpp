#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/memory/longterm.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace longterm = orangutan::memory::longterm;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

class RecoveryFiles {
public:
  RecoveryFiles()
      : directory_(std::filesystem::temp_directory_path() /
                   ("oran-memory-import-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directory(directory_);
  }
  ~RecoveryFiles() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  RecoveryFiles(const RecoveryFiles&) = delete;
  RecoveryFiles& operator=(const RecoveryFiles&) = delete;

  std::string path(std::string_view name) const {
    return (directory_ / name).string();
  }

private:
  std::filesystem::path directory_;
};

storage::Pool open_pool(asio::io_context& io, const std::string& path) {
  auto pool = storage::Pool::open(io.get_executor(), {.path = path, .reader_count = 1});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

longterm::Record note(std::string id = "a", std::string scope = "owner:source") {
  using namespace std::chrono_literals;
  return {
      .key = {.id = std::move(id), .scope_key = std::move(scope)},
      .kind = longterm::RecordKind::feedback,
      .title = "构建约定",
      .body = "Keep complete slices and preserve user data.",
      .created_at = core::Time{core::Time::time_point{1001ms}},
      .updated_at = core::Time{core::Time::time_point{2002ms}},
      .last_read_at = core::Time{core::Time::time_point{3003ms}},
      .importance = 0.12345678901234567,
      .tags = {"recovery", "workflow"},
      .linked_record_ids = {"b"},
  };
}

void require_empty(storage::Connection& connection, std::string_view scope) {
  auto statement = connection.prepare(
      "SELECT 1 FROM longterm_records WHERE scope_key=?1 "
      "UNION ALL SELECT 1 FROM longterm_records_fts WHERE scope_key=?1");
  REQUIRE(statement.has_value());
  REQUIRE(statement->bind_all(scope).has_value());
  auto step = statement->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == storage::StepResult::done);
}

}  // namespace

TEST_CASE("memory recovery preserves a mapped scope and rebuilds search from snapshot records",
          "[memory][longterm][import][preservation]") {
  RecoveryFiles files;
  test::run_async([&files](asio::io_context& io) -> async::Awaitable<void> {
    auto source_pool = open_pool(io, files.path("source.db"));
    longterm::Fts5Backend source{source_pool};
    auto migrated = co_await source.migrate();
    REQUIRE(migrated.has_value());
    auto visible = note();
    auto hidden = note("b");
    hidden.shadow = true;
    hidden.kind = longterm::RecordKind::team;
    hidden.linked_record_ids = {"a"};
    for (const auto& record : {visible, hidden, note("foreign", "owner:other")}) {
      auto saved = co_await source.upsert(record);
      REQUIRE(saved.has_value());
    }
    std::vector<storage::ColumnValue> versions;
    {
      auto writer = co_await source_pool.acquire_writer();
      REQUIRE(writer.has_value());
      auto& connection = writer->connection();
      REQUIRE(connection.execute("DELETE FROM longterm_records_fts").has_value());
      REQUIRE(connection.execute("UPDATE longterm_records SET tags_json='[ \"recovery\", \"workflow\" ]'").has_value());
      REQUIRE(connection.execute("CREATE TABLE host_metadata(value BLOB)").has_value());
      REQUIRE(connection.execute("INSERT INTO host_metadata VALUES(X'0001FF')").has_value());
      auto rows = connection.query("SELECT * FROM schema_versions");
      REQUIRE(rows.has_value());
      REQUIRE(rows->rows.size() == 1);
      versions = rows->rows[0].values;
    }
    auto backup = co_await source.backup_to(files.path("snapshot.db"));
    REQUIRE(backup.has_value());
    auto overwrite = co_await source.backup_to(files.path("snapshot.db"));
    REQUIRE_FALSE(overwrite.has_value());
    REQUIRE(overwrite.error().kind() == core::ErrorKind::invalid_argument);
    auto changed = visible;
    changed.body = "Changed after the snapshot.";
    auto updated = co_await source.upsert(changed);
    REQUIRE(updated.has_value());

    {
      auto target_pool = open_pool(io, files.path("target.db"));
      longterm::Fts5Backend target{target_pool};
      auto initialized = co_await target.migrate();
      REQUIRE(initialized.has_value());
      auto unrelated = co_await target.upsert(note("a", "owner:unrelated"));
      REQUIRE(unrelated.has_value());
      auto imported = co_await target.import_scope(files.path("snapshot.db"), "owner:source", "owner:restored");
      REQUIRE(imported.has_value());
    }
    auto target_pool = open_pool(io, files.path("target.db"));
    longterm::Fts5Backend target{target_pool};
    for (auto expected : {visible, hidden}) {
      expected.key.scope_key = "owner:restored";
      auto actual = co_await target.get(expected.key);
      REQUIRE(actual.has_value());
      REQUIRE(*actual == expected);
    }
    auto hits = co_await target.search({.scope_key = "owner:restored", .text = "recovery", .kinds = {}}, 10);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE(hits->front().record.key.id == "a");
    auto index = co_await longterm::index(target, {.scope_key = "owner:restored"});
    REQUIRE(index.has_value());
    REQUIRE(index->entries.size() == 1);
    auto unchanged = co_await target.get(note("a", "owner:unrelated").key);
    REQUIRE(unchanged.has_value());
    REQUIRE(*unchanged == note("a", "owner:unrelated"));
    auto reader = co_await target_pool.acquire_reader();
    REQUIRE(reader.has_value());
    require_empty(reader->connection(), "owner:source");
    require_empty(reader->connection(), "owner:other");
    auto raw = reader->connection().query("SELECT tags_json FROM longterm_records WHERE scope_key='owner:restored'");
    REQUIRE(raw.has_value());
    REQUIRE(raw->rows.size() == 2);
    REQUIRE(raw->rows[0].values[0] == "[ \"recovery\", \"workflow\" ]");

    auto snapshot = storage::Connection::open(
        {.path = files.path("snapshot.db"), .mode = storage::OpenMode::read_only, .enable_wal = false});
    REQUIRE(snapshot.has_value());
    auto metadata = snapshot->query("SELECT hex(value) FROM host_metadata");
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->rows[0].values[0] == "0001FF");
    auto history = snapshot->query("SELECT * FROM schema_versions");
    REQUIRE(history.has_value());
    REQUIRE(history->rows[0].values == versions);
    auto original_index = snapshot->query("SELECT COUNT(*) FROM longterm_records_fts");
    REQUIRE(original_index.has_value());
    REQUIRE(original_index->rows[0].values[0] == "0");
    auto live = co_await source.get(visible.key);
    REQUIRE(live.has_value());
    REQUIRE(*live == changed);
  });
}

TEST_CASE("memory import refuses occupied scopes including orphan search rows", "[memory][longterm][import]") {
  RecoveryFiles files;
  test::run_async([&files](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, files.path("memory.db"));
    longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    auto saved = co_await backend.upsert(note());
    REQUIRE(saved.has_value());
    auto backup = co_await backend.backup_to(files.path("snapshot.db"));
    REQUIRE(backup.has_value());
    auto unrelated = co_await backend.upsert(note("different-id", "owner:occupied"));
    REQUIRE(unrelated.has_value());
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      auto inserted = writer->connection().execute(
          "INSERT INTO longterm_records_fts(scope_key,record_id,title) VALUES('owner:orphan','orphan','keep')");
      REQUIRE(inserted.has_value());
    }
    for (const auto* scope : {"owner:source", "owner:occupied", "owner:orphan"}) {
      auto conflict = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", scope);
      REQUIRE_FALSE(conflict.has_value());
      REQUIRE(conflict.error().kind() == core::ErrorKind::conflict);
    }
    auto imported = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE(imported.has_value());
    auto duplicate = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().kind() == core::ErrorKind::conflict);
    auto original = co_await backend.get(note().key);
    REQUIRE(original.has_value());
    REQUIRE(*original == note());
    auto occupied = co_await backend.get(note("different-id", "owner:occupied").key);
    REQUIRE(occupied.has_value());
    REQUIRE(*occupied == *unrelated);
    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto orphan = reader->connection().query("SELECT title FROM longterm_records_fts WHERE scope_key='owner:orphan'");
    REQUIRE(orphan.has_value());
    REQUIRE(orphan->rows.size() == 1);
    REQUIRE(orphan->rows[0].values[0] == "keep");
  });
}

TEST_CASE("memory import rejects invalid mappings and missing or unsupported sources", "[memory][longterm][import]") {
  RecoveryFiles files;
  test::run_async([&files](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, files.path("target.db"));
    longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    for (const auto* scope : {"", " ", "owner:\ninvalid"}) {
      auto invalid_source = co_await backend.import_scope(files.path("absent.db"), scope, "owner:new");
      REQUIRE_FALSE(invalid_source.has_value());
      REQUIRE(invalid_source.error().kind() == core::ErrorKind::invalid_argument);
      auto invalid_target = co_await backend.import_scope(files.path("absent.db"), "owner:source", scope);
      REQUIRE_FALSE(invalid_target.has_value());
      REQUIRE(invalid_target.error().kind() == core::ErrorKind::invalid_argument);
    }
    auto absent = co_await backend.import_scope(files.path("absent.db"), "owner:source", "owner:new");
    REQUIRE_FALSE(absent.has_value());
    REQUIRE_FALSE(std::filesystem::exists(files.path("absent.db")));
    auto backup = co_await backend.backup_to(files.path("snapshot.db"));
    REQUIRE(backup.has_value());
    auto missing = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().kind() == core::ErrorKind::not_found);
    {
      auto snapshot = storage::Connection::open({.path = files.path("snapshot.db")});
      REQUIRE(snapshot.has_value());
      REQUIRE(snapshot->execute("UPDATE schema_versions SET version=2").has_value());
    }
    auto unsupported = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE_FALSE(unsupported.has_value());
    REQUIRE(unsupported.error().kind() == core::ErrorKind::invalid_argument);
    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    require_empty(reader->connection(), "owner:new");
  });
}

TEST_CASE("memory import rolls back records and search rows after a late failure", "[memory][longterm][import]") {
  RecoveryFiles files;
  test::run_async([&files](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, files.path("memory.db"));
    longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    for (const auto& record : {note("a"), note("b")}) {
      auto saved = co_await backend.upsert(record);
      REQUIRE(saved.has_value());
    }
    auto backup = co_await backend.backup_to(files.path("snapshot.db"));
    REQUIRE(backup.has_value());
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      REQUIRE(writer->connection().execute(
          "CREATE TRIGGER fail_second BEFORE INSERT ON longterm_records "
          "WHEN NEW.scope_key='owner:new' AND NEW.id='b' BEGIN SELECT RAISE(ABORT,'late failure'); END").has_value());
    }
    auto failed = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE_FALSE(failed.has_value());
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      require_empty(writer->connection(), "owner:new");
      REQUIRE(writer->connection().execute("DROP TRIGGER fail_second").has_value());
    }
    auto retried = co_await backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new");
    REQUIRE(retried.has_value());
    auto hits = co_await backend.search({.scope_key = "owner:new", .text = "recovery", .kinds = {}}, 10);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 2);
  });
}

TEST_CASE("memory import cancellation before writer admission leaves no destination rows",
          "[memory][longterm][import][cancellation]") {
  RecoveryFiles files;
  test::run_async([&files](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, files.path("memory.db"));
    longterm::Fts5Backend backend{pool};
    auto migrated = co_await backend.migrate();
    REQUIRE(migrated.has_value());
    auto saved = co_await backend.upsert(note());
    REQUIRE(saved.has_value());
    auto backup = co_await backend.backup_to(files.path("snapshot.db"));
    REQUIRE(backup.has_value());
    auto held = co_await pool.acquire_writer();
    REQUIRE(held.has_value());
    asio::cancellation_signal signal;
    std::optional<core::Result<void>> result;
    asio::steady_timer joined{io};
    joined.expires_at(std::chrono::steady_clock::time_point::max());
    asio::co_spawn(io,
                   backend.import_scope(files.path("snapshot.db"), "owner:source", "owner:new"),
                   asio::bind_cancellation_slot(signal.slot(), [&result, &joined](std::exception_ptr error,
                                                                                core::Result<void> value) {
                     REQUIRE_FALSE(error);
                     result = std::move(value);
                     joined.cancel();
                   }));
    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(result.has_value());
    signal.emit(asio::cancellation_type::terminal);
    held->release();
    if (!result) {
      asio::error_code error;
      co_await joined.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    require_empty(reader->connection(), "owner:new");
  });
}
