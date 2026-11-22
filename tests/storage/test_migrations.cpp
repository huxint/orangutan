// tests/storage/test_migrations.cpp — storage migration runner coverage.

#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
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

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    REQUIRE_FALSE(ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
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

std::vector<storage::Migration> item_migrations() {
  return {
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
      storage::Migration{.version = 2, .name = "add-name", .sql = "ALTER TABLE items ADD COLUMN name TEXT"},
  };
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  REQUIRE_FALSE(ec);

  std::ofstream output{path, std::ios::binary};
  REQUIRE(output.is_open());
  output << contents;
  REQUIRE(output.good());
}

}  // namespace

TEST_CASE("load_migrations_from_directory loads sorted numbered SQL files", "[unit][storage][migrations]") {
  TempDir dir{"oran-storage-migration-files"};
  write_file(dir.path() / "0002-add-name.sql", "ALTER TABLE items ADD COLUMN name TEXT");
  write_file(dir.path() / "0001-create-items.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");

  auto migrations = storage::load_migrations_from_directory(dir.string());

  REQUIRE(migrations.has_value());
  REQUIRE(migrations->size() == 2);
  REQUIRE((*migrations)[0].version == 1);
  REQUIRE((*migrations)[0].name == "create-items");
  REQUIRE((*migrations)[0].sql == "CREATE TABLE items(id INTEGER PRIMARY KEY)");
  REQUIRE((*migrations)[1].version == 2);
  REQUIRE((*migrations)[1].name == "add-name");
  REQUIRE((*migrations)[1].sql == "ALTER TABLE items ADD COLUMN name TEXT");

  auto connection = open_memory();
  auto report = storage::run_migrations(connection, *migrations);
  REQUIRE(report.has_value());
  REQUIRE(report->current_version == 2);
  REQUIRE(connection.execute("INSERT INTO items(id, name) VALUES (1, 'alpha')").has_value());
}

TEST_CASE("run_migrations_from_directory applies file migrations and reruns as a no-op",
          "[unit][storage][migrations]") {
  TempDir dir{"oran-storage-migration-run-files"};
  write_file(dir.path() / "0001-create-items.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");
  write_file(dir.path() / "0002-add-name.sql", "ALTER TABLE items ADD COLUMN name TEXT");

  auto connection = open_memory();
  auto first = storage::run_migrations_from_directory(connection, dir.string());
  REQUIRE(first.has_value());
  REQUIRE(first->previous_version == 0);
  REQUIRE(first->current_version == 2);
  REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2});

  auto second = storage::run_migrations_from_directory(connection, dir.string());
  REQUIRE(second.has_value());
  REQUIRE(second->previous_version == 2);
  REQUIRE(second->current_version == 2);
  REQUIRE(second->applied_versions.empty());
}

TEST_CASE("load_migrations_from_directory rejects bad directories and filenames", "[unit][storage][migrations]") {
  SECTION("empty directory path") {
    auto result = storage::load_migrations_from_directory("");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("missing directory") {
    const auto missing = std::filesystem::temp_directory_path() /
                         ("oran-storage-migration-missing-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto result = storage::load_migrations_from_directory(missing.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  }

  SECTION("path is not a directory") {
    TempDir dir{"oran-storage-migration-not-dir"};
    const auto path = dir.path() / "0001-create.sql";
    write_file(path, "CREATE TABLE items(id INTEGER PRIMARY KEY)");
    auto result = storage::load_migrations_from_directory(path.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty directory") {
    TempDir dir{"oran-storage-migration-empty"};
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("non-sql regular file") {
    TempDir dir{"oran-storage-migration-non-sql"};
    write_file(dir.path() / "README.md", "not a migration");
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("malformed migration filename") {
    TempDir dir{"oran-storage-migration-bad-name"};
    write_file(dir.path() / "1-create.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("gapped versions") {
    TempDir dir{"oran-storage-migration-gap"};
    write_file(dir.path() / "0001-create.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");
    write_file(dir.path() / "0003-add-name.sql", "ALTER TABLE items ADD COLUMN name TEXT");
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("duplicate versions") {
    TempDir dir{"oran-storage-migration-duplicate"};
    write_file(dir.path() / "0001-create.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");
    write_file(dir.path() / "0001-again.sql", "CREATE TABLE duplicate_items(id INTEGER PRIMARY KEY)");
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty sql file") {
    TempDir dir{"oran-storage-migration-empty-sql"};
    write_file(dir.path() / "0001-empty.sql", "");
    auto result = storage::load_migrations_from_directory(dir.string());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run_migrations_from_directory rejects invalid files before touching the database",
          "[unit][storage][migrations]") {
  TempDir dir{"oran-storage-migration-preflight"};
  write_file(dir.path() / "0001-create.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY)");
  write_file(dir.path() / "0003-gap.sql", "CREATE TABLE gap_table(id INTEGER PRIMARY KEY)");

  auto connection = open_memory();
  auto result = storage::run_migrations_from_directory(connection, dir.string());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);

  auto schema = connection.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'schema_versions'");
  REQUIRE(schema.has_value());
  REQUIRE(schema->rows.empty());
}

TEST_CASE("run_migrations applies pending migrations and records versions", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();

  auto report = storage::run_migrations(connection, migrations);

  REQUIRE(report.has_value());
  REQUIRE(report->previous_version == 0);
  REQUIRE(report->current_version == 2);
  REQUIRE(report->applied_versions == std::vector<std::int64_t>{1, 2});

  REQUIRE(connection.execute("INSERT INTO items(id, name) VALUES (1, 'alpha')").has_value());
  auto versions = connection.query("SELECT version, name FROM schema_versions ORDER BY version");
  REQUIRE(versions.has_value());
  REQUIRE(versions->rows.size() == 2);
  REQUIRE(versions->rows[0].values[0] == "1");
  REQUIRE(versions->rows[0].values[1] == "create-items");
  REQUIRE(versions->rows[1].values[0] == "2");
  REQUIRE(versions->rows[1].values[1] == "add-name");
}

TEST_CASE("run_migrations reruns as an idempotent no-op", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();
  REQUIRE(storage::run_migrations(connection, migrations).has_value());

  auto report = storage::run_migrations(connection, migrations);

  REQUIRE(report.has_value());
  REQUIRE(report->previous_version == 2);
  REQUIRE(report->current_version == 2);
  REQUIRE(report->applied_versions.empty());
}

TEST_CASE("run_migrations reconciles concurrent runners on the same database", "[unit][storage][migrations]") {
  TempDb db{"oran-storage-migration-concurrent"};
  auto first = storage::Connection::open(storage::ConnectionOptions{.path = db.string(), .busy_timeout_ms = 5000});
  auto second = storage::Connection::open(storage::ConnectionOptions{.path = db.string(), .busy_timeout_ms = 5000});
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  const std::vector<storage::Migration> migrations{
      storage::Migration{
          .version = 1,
          .name = "create-items",
          .sql = R"sql(
            CREATE TABLE items(id INTEGER PRIMARY KEY);
            WITH RECURSIVE ids(value) AS (
              SELECT 1 UNION ALL SELECT value + 1 FROM ids WHERE value < 20000
            ) INSERT INTO items(id) SELECT value FROM ids;
          )sql",
      },
      storage::Migration{.version = 2, .name = "add-name", .sql = "ALTER TABLE items ADD COLUMN name TEXT"},
  };

  std::barrier start{3};
  std::optional<core::Result<storage::MigrationReport>> first_result;
  std::optional<core::Result<storage::MigrationReport>> second_result;
  std::jthread first_runner{[&] {
    start.arrive_and_wait();
    first_result = storage::run_migrations(*first, migrations);
  }};
  std::jthread second_runner{[&] {
    start.arrive_and_wait();
    second_result = storage::run_migrations(*second, migrations);
  }};
  start.arrive_and_wait();
  first_runner.join();
  second_runner.join();

  REQUIRE(first_result.has_value());
  REQUIRE(second_result.has_value());
  REQUIRE(first_result->has_value());
  REQUIRE(second_result->has_value());
  REQUIRE((*first_result)->current_version == 2);
  REQUIRE((*second_result)->current_version == 2);

  auto versions = first->query("SELECT version FROM schema_versions ORDER BY version");
  REQUIRE(versions.has_value());
  REQUIRE(versions->rows.size() == 2);
}

TEST_CASE("run_migrations rejects invalid migration lists before applying", "[unit][storage][migrations]") {
  auto connection = open_memory();

  SECTION("non-positive version") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 0, .name = "bad", .sql = "CREATE TABLE bad(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("gap") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
        storage::Migration{.version = 3, .name = "three", .sql = "CREATE TABLE three(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("duplicate") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
        storage::Migration{.version = 1, .name = "again", .sql = "CREATE TABLE again(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty name") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "", .sql = "CREATE TABLE one(id INTEGER)"},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("empty sql") {
    const std::vector<storage::Migration> migrations{
        storage::Migration{.version = 1, .name = "empty", .sql = ""},
    };
    auto result = storage::run_migrations(connection, migrations);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("run_migrations rolls back a failed migration", "[unit][storage][migrations]") {
  auto connection = open_memory();
  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "create-stable", .sql = "CREATE TABLE stable(id INTEGER)"},
      storage::Migration{
          .version = 2,
          .name = "bad-second",
          .sql = "CREATE TABLE transient(id INTEGER); INSERT INTO missing_table(id) VALUES (1)",
      },
  };

  auto result = storage::run_migrations(connection, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::storage);

  auto versions = connection.query("SELECT version FROM schema_versions ORDER BY version");
  REQUIRE(versions.has_value());
  REQUIRE(versions->rows.size() == 1);
  REQUIRE(versions->rows[0].values[0] == "1");

  auto transient = connection.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'transient'");
  REQUIRE(transient.has_value());
  REQUIRE(transient->rows.empty());
}

TEST_CASE("run_migrations rejects databases newer than the migration set", "[unit][storage][migrations]") {
  auto connection = open_memory();
  auto migrations = item_migrations();
  REQUIRE(storage::run_migrations(connection, migrations).has_value());

  const std::vector<storage::Migration> older_binary{
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
  };
  auto result = storage::run_migrations(connection, older_binary);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("run_migrations detects non-contiguous recorded versions", "[unit][storage][migrations]") {
  auto connection = open_memory();
  REQUIRE(connection.execute("CREATE TABLE schema_versions(version INTEGER PRIMARY KEY, name TEXT, applied_at TEXT)")
              .has_value());
  REQUIRE(connection.execute("INSERT INTO schema_versions(version, name, applied_at) VALUES (1, 'one', 'now')")
              .has_value());
  REQUIRE(connection.execute("INSERT INTO schema_versions(version, name, applied_at) VALUES (3, 'three', 'now')")
              .has_value());

  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "one", .sql = "CREATE TABLE one(id INTEGER)"},
      storage::Migration{.version = 2, .name = "two", .sql = "CREATE TABLE two(id INTEGER)"},
      storage::Migration{.version = 3, .name = "three", .sql = "CREATE TABLE three(id INTEGER)"},
  };
  auto result = storage::run_migrations(connection, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("run_migrations fails expected-only on read-only databases that need schema setup",
          "[unit][storage][migrations]") {
  TempDb db{"oran-storage-migration-readonly"};
  {
    auto writable = storage::Connection::open(storage::ConnectionOptions{.path = db.string(), .enable_wal = false});
    REQUIRE(writable.has_value());
  }

  auto readonly = storage::Connection::open(storage::ConnectionOptions{
      .path = db.string(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
      .enforce_foreign_keys = false,
  });
  REQUIRE(readonly.has_value());

  const std::vector<storage::Migration> migrations{
      storage::Migration{.version = 1, .name = "create-items", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
  };
  auto result = storage::run_migrations(*readonly, migrations);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::storage);
}
