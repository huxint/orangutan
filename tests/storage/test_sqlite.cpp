// tests/storage/test_sqlite.cpp — expected-only SQLite core coverage.

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/storage.hpp>

namespace core = orangutan::core;
namespace storage = orangutan::storage;

namespace {

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Connection open_memory() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  REQUIRE(connection.has_value());
  return std::move(*connection);
}

}  // namespace

TEST_CASE("Connection opens a file-backed database with WAL and foreign keys", "[unit][storage][sqlite]") {
  TempDb db{"oran-storage-open"};
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = db.string()});

  REQUIRE(connection.has_value());
  REQUIRE(connection->is_open());

  auto journal = connection->query("PRAGMA journal_mode");
  REQUIRE(journal.has_value());
  REQUIRE(journal->rows.size() == 1);
  REQUIRE(journal->rows[0].values[0] == "wal");

  auto foreign_keys = connection->query("PRAGMA foreign_keys");
  REQUIRE(foreign_keys.has_value());
  REQUIRE(foreign_keys->rows.size() == 1);
  REQUIRE(foreign_keys->rows[0].values[0] == "1");
}

TEST_CASE("Connection rejects empty paths and empty SQL", "[unit][storage][sqlite]") {
  auto missing_path = storage::Connection::open(storage::ConnectionOptions{.path = ""});
  REQUIRE_FALSE(missing_path.has_value());
  REQUIRE(missing_path.error().kind() == core::ErrorKind::invalid_argument);

  auto connection = open_memory();
  auto empty_execute = connection.execute("");
  auto empty_prepare = connection.prepare("");

  REQUIRE_FALSE(empty_execute.has_value());
  REQUIRE(empty_execute.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE_FALSE(empty_prepare.has_value());
  REQUIRE(empty_prepare.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("execute and query round-trip rows as text values", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, note TEXT)").has_value());
  REQUIRE(connection.execute("INSERT INTO items(id, name, note) VALUES (2, 'beta', NULL), (1, 'alpha', 'first')")
              .has_value());

  auto result = connection.query("SELECT id, name, note FROM items ORDER BY id");

  REQUIRE(result.has_value());
  REQUIRE(result->columns == std::vector<std::string>{"id", "name", "note"});
  REQUIRE(result->rows.size() == 2);
  REQUIRE(result->rows[0].values[0] == "1");
  REQUIRE(result->rows[0].values[1] == "alpha");
  REQUIRE(result->rows[0].values[2] == "first");
  REQUIRE(result->rows[1].values[0] == "2");
  REQUIRE(result->rows[1].values[1] == "beta");
  REQUIRE_FALSE(result->rows[1].values[2].has_value());
}

TEST_CASE("prepared statements bind and read typed values", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(
      connection.execute("CREATE TABLE metrics(id INTEGER PRIMARY KEY, label TEXT, score REAL, note TEXT, raw BLOB)")
          .has_value());

  auto insert = connection.prepare("INSERT INTO metrics(id, label, score, note, raw) VALUES (?, ?, ?, ?, ?)");
  REQUIRE(insert.has_value());
  const std::vector<std::byte> raw{std::byte{0x01}, std::byte{0x02}, std::byte{0xff}};
  REQUIRE(insert->bind_int64(1, 42).has_value());
  REQUIRE(insert->bind_text(2, "latency").has_value());
  REQUIRE(insert->bind_double(3, 12.5).has_value());
  REQUIRE(insert->bind_null(4).has_value());
  REQUIRE(insert->bind_blob(5, std::span<const std::byte>{raw}).has_value());
  auto inserted = insert->step();
  REQUIRE(inserted.has_value());
  REQUIRE(*inserted == storage::StepResult::done);

  auto select = connection.prepare("SELECT id, label, score, note, raw FROM metrics");
  REQUIRE(select.has_value());
  auto row = select->step();
  REQUIRE(row.has_value());
  REQUIRE(*row == storage::StepResult::row);

  auto id = select->column_int64(0);
  auto label = select->column_text(1);
  auto score = select->column_double(2);
  auto note = select->column_text(3);
  auto blob = select->column_blob(4);
  REQUIRE(id.has_value());
  REQUIRE(*id == 42);
  REQUIRE(label.has_value());
  REQUIRE(*label == "latency");
  REQUIRE(score.has_value());
  REQUIRE(*score == 12.5);
  REQUIRE(note.has_value());
  REQUIRE_FALSE(note->has_value());
  REQUIRE(blob.has_value());
  REQUIRE(blob->has_value());
  REQUIRE(**blob == raw);
}

TEST_CASE("column readers require a current row", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE metrics(id INTEGER PRIMARY KEY)").has_value());
  REQUIRE(connection.execute("INSERT INTO metrics(id) VALUES (7)").has_value());

  auto select = connection.prepare("SELECT id FROM metrics");
  REQUIRE(select.has_value());

  auto before_step = select->column_int64(0);
  REQUIRE_FALSE(before_step.has_value());
  REQUIRE(before_step.error().kind() == core::ErrorKind::conflict);
  auto before_double = select->column_double(0);
  REQUIRE_FALSE(before_double.has_value());
  REQUIRE(before_double.error().kind() == core::ErrorKind::conflict);

  auto row = select->step();
  REQUIRE(row.has_value());
  REQUIRE(*row == storage::StepResult::row);
  auto current = select->column_int64(0);
  REQUIRE(current.has_value());
  REQUIRE(*current == 7);

  auto done = select->step();
  REQUIRE(done.has_value());
  REQUIRE(*done == storage::StepResult::done);

  auto after_done = select->column_text(0);
  REQUIRE_FALSE(after_done.has_value());
  REQUIRE(after_done.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("statement reset and clear_bindings allow reuse", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE logs(id INTEGER PRIMARY KEY, message TEXT)").has_value());

  auto insert = connection.prepare("INSERT INTO logs(id, message) VALUES (?, ?)");
  REQUIRE(insert.has_value());

  REQUIRE(insert->bind_int64(1, 1).has_value());
  REQUIRE(insert->bind_text(2, "one").has_value());
  REQUIRE(insert->step().has_value());
  REQUIRE(insert->reset().has_value());
  REQUIRE(insert->clear_bindings().has_value());

  REQUIRE(insert->bind_int64(1, 2).has_value());
  REQUIRE(insert->bind_text(2, "two").has_value());
  REQUIRE(insert->step().has_value());

  auto result = connection.query("SELECT message FROM logs ORDER BY id");
  REQUIRE(result.has_value());
  REQUIRE(result->rows.size() == 2);
  REQUIRE(result->rows[0].values[0] == "one");
  REQUIRE(result->rows[1].values[0] == "two");
}

TEST_CASE("SQLite failures return storage errors with context", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE unique_items(id INTEGER PRIMARY KEY)").has_value());
  REQUIRE(connection.execute("INSERT INTO unique_items(id) VALUES (1)").has_value());

  auto duplicate = connection.execute("INSERT INTO unique_items(id) VALUES (1)");
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::storage);
  REQUIRE_FALSE(duplicate.error().context().empty());
}

TEST_CASE("prepared statement keeps database alive after connection object closes", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE values_table(value TEXT)").has_value());
  auto insert = connection.prepare("INSERT INTO values_table(value) VALUES ('kept')");
  REQUIRE(insert.has_value());

  connection.close();

  auto step = insert->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == storage::StepResult::done);
  REQUIRE_FALSE(connection.is_open());
}

TEST_CASE("read-only connections reject writes", "[unit][storage][sqlite]") {
  TempDb db{"oran-storage-readonly"};
  {
    auto writable = storage::Connection::open(storage::ConnectionOptions{.path = db.string()});
    REQUIRE(writable.has_value());
    REQUIRE(writable->execute("CREATE TABLE items(id INTEGER PRIMARY KEY)").has_value());
  }

  auto readonly = storage::Connection::open(storage::ConnectionOptions{
      .path = db.string(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
      .enforce_foreign_keys = false,
  });
  REQUIRE(readonly.has_value());

  auto write = readonly->execute("INSERT INTO items(id) VALUES (1)");
  REQUIRE_FALSE(write.has_value());
  REQUIRE(write.error().kind() == core::ErrorKind::storage);
}

TEST_CASE("bind_all binds positional values by type", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE items(a TEXT, b INTEGER, c REAL, d INTEGER, e BLOB, f TEXT, g TEXT)")
              .has_value());
  auto insert = connection.prepare("INSERT INTO items VALUES (?, ?, ?, ?, ?, ?, ?)");
  REQUIRE(insert.has_value());
  const auto blob = std::array{std::byte{0x01}, std::byte{0x02}};
  REQUIRE(insert
              ->bind_all(std::string{"text"},
                         7,
                         0.5,
                         true,
                         blob,
                         std::optional<std::string_view>{},
                         storage::Null{})
              .has_value());
  REQUIRE(insert->step() == storage::StepResult::done);

  auto result = connection.query("SELECT a, b, c, d, typeof(e), length(e), f IS NULL, g IS NULL FROM items");
  REQUIRE(result.has_value());
  REQUIRE(result->rows.size() == 1);
  const auto& row = result->rows[0].values;
  CHECK(row[0] == "text");
  CHECK(row[1] == "7");
  CHECK(row[2] == "0.5");
  CHECK(row[3] == "1");
  CHECK(row[4] == "blob");
  CHECK(row[5] == "2");
  CHECK(row[6] == "1");
  CHECK(row[7] == "1");
}

TEST_CASE("Transaction rolls back unless committed", "[unit][storage][sqlite]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE items(id INTEGER)").has_value());
  {
    auto transaction = storage::Transaction::begin(connection);
    REQUIRE(transaction.has_value());
    REQUIRE(connection.execute("INSERT INTO items VALUES (1)").has_value());
  }
  {
    auto transaction = storage::Transaction::begin(connection);
    REQUIRE(transaction.has_value());
    REQUIRE(connection.execute("INSERT INTO items VALUES (2)").has_value());
    REQUIRE(transaction->commit().has_value());
  }
  auto result = connection.query("SELECT id FROM items");
  REQUIRE(result.has_value());
  REQUIRE(result->rows.size() == 1);
  CHECK(result->rows[0].values[0] == "2");
}
