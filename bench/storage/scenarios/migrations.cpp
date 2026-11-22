// bench/storage/scenarios/migrations.cpp
//
// A-vs-B comparison: cold migration apply vs. no-op migration check.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kMigrationCount = 16;

std::string make_temp_dir_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  return path.string();
}

class TempDir {
public:
  explicit TempDir(std::string_view tag) : path_{make_temp_dir_path(tag)} {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    if (ec) {
      std::abort();
    }
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

private:
  std::string path_;
};

std::vector<storage::Migration> make_migrations() {
  std::vector<storage::Migration> migrations;
  migrations.reserve(kMigrationCount);
  for (int i = 1; i <= kMigrationCount; ++i) {
    migrations.push_back(storage::Migration{
        .version = i,
        .name = std::format("create-table-{}", i),
        .sql = std::format("CREATE TABLE table_{}(id INTEGER PRIMARY KEY, value TEXT)", i),
    });
  }
  return migrations;
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  if (!output.is_open()) {
    std::abort();
  }
  output << contents;
  if (!output.good()) {
    std::abort();
  }
}

void write_migration_files(const std::filesystem::path& directory) {
  for (int i = 1; i <= kMigrationCount; ++i) {
    write_file(directory / std::format("{:04d}-create-table-{}.sql", i, i),
               std::format("CREATE TABLE table_{}(id INTEGER PRIMARY KEY, value TEXT)", i));
  }
}

storage::Connection make_connection() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  if (!connection) {
    std::abort();
  }
  return std::move(*connection);
}

[[gnu::noinline]] std::int64_t cold_apply(const std::vector<storage::Migration>& migrations) {
  auto connection = make_connection();
  auto report = storage::run_migrations(connection, migrations);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

[[gnu::noinline]] std::int64_t file_cold_apply(std::string_view directory) {
  auto connection = make_connection();
  auto report = storage::run_migrations_from_directory(connection, directory);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

[[gnu::noinline]] std::int64_t noop_check(storage::Connection& connection,
                                          const std::vector<storage::Migration>& migrations) {
  auto report = storage::run_migrations(connection, migrations);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

[[gnu::noinline]] std::int64_t file_noop_check(storage::Connection& connection, std::string_view directory) {
  auto report = storage::run_migrations_from_directory(connection, directory);
  if (!report) {
    std::abort();
  }
  return report->current_version + static_cast<std::int64_t>(report->applied_versions.size());
}

}  // namespace

void register_migrations(ankerl::nanobench::Bench& bench) {
  const auto migrations = make_migrations();
  auto migrated = make_connection();
  if (!storage::run_migrations(migrated, migrations)) {
    std::abort();
  }
  TempDir migration_files{"migration-files"};
  write_migration_files(migration_files.path());
  auto file_migrated = make_connection();
  if (!storage::run_migrations_from_directory(file_migrated, migration_files.path())) {
    std::abort();
  }

  bench.run("storage.migrations_cold_apply", [&] {
    auto result = cold_apply(migrations);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.migrations_noop_check", [&] {
    auto result = noop_check(migrated, migrations);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.minEpochIterations(1200);
  bench.run("storage.migrations_file_cold_apply", [&] {
    auto result = file_cold_apply(migration_files.path());
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.migrations_file_noop_check", [&] {
    auto result = file_noop_check(file_migrated, migration_files.path());
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.minEpochIterations(1000);
}

}  // namespace orangutan::bench
