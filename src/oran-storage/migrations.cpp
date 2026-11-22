// src/oran-storage/migrations.cpp — SQLite migration runner.

#include <oran/storage/migrations.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kCreateSchemaVersionsSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_versions(
  version INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  applied_at TEXT NOT NULL
)
)sql";

constexpr std::string_view kReadSchemaVersionsSql = "SELECT version FROM schema_versions ORDER BY version";

constexpr std::string_view kInsertSchemaVersionSql = R"sql(
INSERT INTO schema_versions(version, name, applied_at)
VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
)sql";

constexpr std::string_view kMigrationFileExtension = ".sql";
constexpr std::size_t kMigrationFileVersionWidth = 4;
// Cap an individual migration file at 8 MiB. Migration SQL is repo-vendored
// and small by nature; the bound exists to refuse a corrupt or hostile file
// before allocating gigabytes of string buffer.
constexpr std::uintmax_t kMaxMigrationFileBytes = 8ULL * 1024 * 1024;

[[nodiscard]] core::Error attach_migration_context(core::Error error, const Migration& migration) {
  error.with("migration_version", std::to_string(migration.version)).with("migration_name", migration.name);
  return error;
}

[[nodiscard]] core::Error io_error(std::string message, const std::filesystem::path& path, std::error_code ec = {}) {
  auto error = core::Error::io(std::move(message));
  error.with("path", path.string());
  if (ec) {
    error.with("system_error", ec.message());
  }
  return error;
}

[[nodiscard]] core::Error invalid_migration_file(std::string message, const std::filesystem::path& path) {
  auto error = core::Error::invalid_argument(std::move(message));
  error.with("path", path.string());
  return error;
}

[[nodiscard]] core::Result<void> validate_migration_list(std::span<const Migration> migrations) {
  std::int64_t expected_version = 1;
  for (const auto& migration : migrations) {
    if (migration.version <= 0) {
      return std::unexpected(core::Error::invalid_argument("migration version must be positive")
                                 .with("migration_version", std::to_string(migration.version)));
    }
    if (migration.version != expected_version) {
      return std::unexpected(core::Error::invalid_argument("migration versions must be contiguous from 1")
                                 .with("expected_version", std::to_string(expected_version))
                                 .with("actual_version", std::to_string(migration.version)));
    }
    if (migration.name.empty()) {
      return std::unexpected(core::Error::invalid_argument("migration name must not be empty")
                                 .with("migration_version", std::to_string(migration.version)));
    }
    if (migration.sql.empty()) {
      return std::unexpected(core::Error::invalid_argument("migration sql must not be empty")
                                 .with("migration_version", std::to_string(migration.version))
                                 .with("migration_name", migration.name));
    }
    ++expected_version;
  }
  return {};
}

[[nodiscard]] core::Result<std::int64_t> parse_migration_file_version(std::string_view text,
                                                                      const std::filesystem::path& path) {
  if (text.size() != kMigrationFileVersionWidth ||
      !std::ranges::all_of(text, [](char ch) { return ch >= '0' && ch <= '9'; })) {
    return std::unexpected(invalid_migration_file("migration filename version must be four digits", path));
  }

  std::int64_t version{};
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, version);
  if (ec != std::errc{} || ptr != last || version <= 0) {
    return std::unexpected(invalid_migration_file("migration filename version must be positive", path));
  }
  return version;
}

[[nodiscard]] core::Result<Migration> load_migration_file(const std::filesystem::path& path) {
  const auto filename = path.filename().string();
  if (!filename.ends_with(kMigrationFileExtension)) {
    return std::unexpected(invalid_migration_file("migration file must use the .sql extension", path));
  }

  const auto stem = filename.substr(0, filename.size() - kMigrationFileExtension.size());
  const auto separator = stem.find('-');
  if (separator != kMigrationFileVersionWidth || separator + 1 == stem.size()) {
    return std::unexpected(invalid_migration_file("migration filename must match 0001-name.sql", path));
  }

  auto version = parse_migration_file_version(std::string_view{stem}.substr(0, separator), path);
  if (!version) {
    return std::unexpected(version.error());
  }

  std::error_code size_ec;
  const auto file_bytes = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    return std::unexpected(io_error("failed to stat migration file", path, size_ec));
  }
  if (file_bytes > kMaxMigrationFileBytes) {
    auto error = core::Error::invalid_argument("migration file exceeds size limit");
    error.with("path", path.string())
        .with("size", std::to_string(file_bytes))
        .with("max_bytes", std::to_string(kMaxMigrationFileBytes));
    return std::unexpected(std::move(error));
  }

  std::ifstream input{path, std::ios::binary};
  if (!input.is_open()) {
    return std::unexpected(io_error("failed to open migration file", path));
  }

  std::string sql;
  sql.reserve(static_cast<std::size_t>(file_bytes));
  sql.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
  if (input.bad()) {
    return std::unexpected(io_error("failed to read migration file", path));
  }

  return Migration{
      .version = *version,
      .name = stem.substr(separator + 1),
      .sql = std::move(sql),
  };
}

[[nodiscard]] core::Result<std::int64_t> parse_version(std::string_view text) {
  std::int64_t version{};
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, version);
  if (ec != std::errc{} || ptr != last) {
    return std::unexpected(
        core::Error::storage("schema_versions contains an invalid version").with("version_text", std::string{text}));
  }
  return version;
}

[[nodiscard]] core::Result<std::vector<std::int64_t>> read_applied_versions(Connection& connection) {
  auto rows = connection.query(kReadSchemaVersionsSql);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<std::int64_t> versions;
  versions.reserve(rows->rows.size());
  for (const auto& row : rows->rows) {
    if (row.values.empty() || !row.values.front()) {
      return std::unexpected(core::Error::storage("schema_versions row is missing version"));
    }
    auto version = parse_version(*row.values.front());
    if (!version) {
      return std::unexpected(version.error());
    }
    versions.push_back(*version);
  }
  return versions;
}

[[nodiscard]] core::Result<std::int64_t> validate_applied_versions(std::span<const std::int64_t> versions,
                                                                   std::size_t migration_count) {
  std::int64_t expected_version = 1;
  for (const auto version : versions) {
    if (version != expected_version) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "schema_versions is not contiguous"}
                                 .with("expected_version", std::to_string(expected_version))
                                 .with("actual_version", std::to_string(version)));
    }
    ++expected_version;
  }

  const auto current_version = versions.empty() ? 0 : versions.back();
  if (current_version > static_cast<std::int64_t>(migration_count)) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "database schema is newer than migration set"}
                               .with("current_version", std::to_string(current_version))
                               .with("migration_count", std::to_string(migration_count)));
  }
  return current_version;
}

[[nodiscard]] core::Result<void> record_migration(Connection& connection, const Migration& migration) {
  auto insert = connection.prepare(kInsertSchemaVersionSql);
  if (!insert) {
    return std::unexpected(insert.error());
  }
  if (auto bound = insert->bind_all(migration.version, migration.name); !bound) {
    return std::unexpected(bound.error());
  }
  auto stepped = insert->step();
  if (!stepped) {
    return std::unexpected(stepped.error());
  }
  if (*stepped != StepResult::done) {
    return std::unexpected(core::Error::storage("schema version insert returned a row"));
  }
  return {};
}

[[nodiscard]] core::Result<std::optional<std::string>> recorded_migration_name(Connection& connection,
                                                                               std::int64_t version) {
  auto query = connection.prepare("SELECT name FROM schema_versions WHERE version = ?");
  if (!query) {
    return std::unexpected(query.error());
  }
  if (auto bound = query->bind_all(version); !bound) {
    return std::unexpected(bound.error());
  }
  auto stepped = query->step();
  if (!stepped) {
    return std::unexpected(stepped.error());
  }
  if (*stepped == StepResult::done) {
    return std::nullopt;
  }
  auto name = query->column_text(0);
  if (!name) {
    return std::unexpected(name.error());
  }
  if (!*name) {
    return std::unexpected(core::Error::storage("schema_versions row is missing migration name")
                               .with("migration_version", std::to_string(version)));
  }
  return std::move(**name);
}

[[nodiscard]] core::Result<bool>
apply_migration(Connection& connection, const Migration& migration, std::size_t migration_count) {
  auto transaction = Transaction::begin(connection);
  if (!transaction) {
    return std::unexpected(attach_migration_context(transaction.error(), migration));
  }

  auto applied_versions = read_applied_versions(connection);
  if (!applied_versions) {
    return std::unexpected(attach_migration_context(applied_versions.error(), migration));
  }
  auto current_version = validate_applied_versions(*applied_versions, migration_count);
  if (!current_version) {
    return std::unexpected(attach_migration_context(current_version.error(), migration));
  }

  auto recorded_name = recorded_migration_name(connection, migration.version);
  if (!recorded_name) {
    return std::unexpected(attach_migration_context(recorded_name.error(), migration));
  }
  if (recorded_name->has_value()) {
    if (**recorded_name != migration.name) {
      auto conflict =
          core::Error{core::ErrorKind::conflict, "migration version was applied with a different name"}.with(
              "recorded_name",
              **recorded_name);
      return std::unexpected(attach_migration_context(std::move(conflict), migration));
    }
    if (auto committed = transaction->commit(); !committed) {
      return std::unexpected(attach_migration_context(committed.error(), migration));
    }
    return false;
  }

  if (auto applied = connection.execute(migration.sql); !applied) {
    return std::unexpected(attach_migration_context(applied.error(), migration));
  }

  if (auto recorded = record_migration(connection, migration); !recorded) {
    return std::unexpected(attach_migration_context(recorded.error(), migration));
  }

  if (auto committed = transaction->commit(); !committed) {
    return std::unexpected(attach_migration_context(committed.error(), migration));
  }
  return true;
}

}  // namespace

core::Result<MigrationReport> run_migrations(Connection& connection, std::span<const Migration> migrations) {
  if (auto valid = validate_migration_list(migrations); !valid) {
    return std::unexpected(valid.error());
  }

  if (auto schema = connection.execute(kCreateSchemaVersionsSql); !schema) {
    return std::unexpected(schema.error());
  }

  auto applied_versions = read_applied_versions(connection);
  if (!applied_versions) {
    return std::unexpected(applied_versions.error());
  }

  auto current_version = validate_applied_versions(std::span<const std::int64_t>{*applied_versions}, migrations.size());
  if (!current_version) {
    return std::unexpected(current_version.error());
  }

  MigrationReport report{
      .previous_version = *current_version,
      .current_version = *current_version,
      .applied_versions = {},
  };

  for (const auto& migration : migrations) {
    if (migration.version <= report.current_version) {
      continue;
    }
    auto applied = apply_migration(connection, migration, migrations.size());
    if (!applied) {
      return std::unexpected(applied.error());
    }
    report.current_version = migration.version;
    if (*applied) {
      report.applied_versions.push_back(migration.version);
    }
  }

  return report;
}

core::Result<std::vector<Migration>> load_migrations_from_directory(std::string_view directory) {
  if (directory.empty()) {
    return std::unexpected(core::Error::invalid_argument("migration directory must not be empty"));
  }

  const std::filesystem::path root{std::string{directory}};
  std::error_code ec;
  const auto status = std::filesystem::status(root, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      auto error = core::Error::not_found("migration directory does not exist");
      error.with("path", root.string());
      return std::unexpected(std::move(error));
    }
    return std::unexpected(io_error("failed to inspect migration directory", root, ec));
  }
  if (!std::filesystem::exists(status)) {
    auto error = core::Error::not_found("migration directory does not exist");
    error.with("path", root.string());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::is_directory(status)) {
    return std::unexpected(invalid_migration_file("migration path must be a directory", root));
  }

  std::vector<Migration> migrations;
  std::filesystem::directory_iterator iterator{root, ec};
  if (ec) {
    return std::unexpected(io_error("failed to open migration directory", root, ec));
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto entry = *iterator;
    const auto path = entry.path();
    const auto regular = entry.is_regular_file(ec);
    if (ec) {
      return std::unexpected(io_error("failed to inspect migration file", path, ec));
    }
    if (regular) {
      auto migration = load_migration_file(path);
      if (!migration) {
        return std::unexpected(migration.error());
      }
      migrations.push_back(std::move(*migration));
    }

    iterator.increment(ec);
    if (ec) {
      return std::unexpected(io_error("failed to read migration directory", root, ec));
    }
  }

  if (migrations.empty()) {
    auto error = core::Error::invalid_argument("migration directory contains no migration files");
    error.with("path", root.string());
    return std::unexpected(std::move(error));
  }

  std::ranges::sort(migrations, {}, &Migration::version);
  if (auto valid = validate_migration_list(migrations); !valid) {
    return std::unexpected(valid.error());
  }
  return migrations;
}

core::Result<MigrationReport> run_migrations_from_directory(Connection& connection, std::string_view directory) {
  auto migrations = load_migrations_from_directory(directory);
  if (!migrations) {
    return std::unexpected(migrations.error());
  }
  return run_migrations(connection, std::span<const Migration>{*migrations});
}

}  // namespace orangutan::storage
