// include/oran/storage/migrations.hpp — expected-only SQLite migrations.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::storage {

class Connection;

struct Migration {
  std::int64_t version{};
  std::string name;
  std::string sql;
};

struct MigrationReport {
  std::int64_t previous_version{};
  std::int64_t current_version{};
  std::vector<std::int64_t> applied_versions;
};

[[nodiscard]] core::Result<MigrationReport> run_migrations(Connection& connection,
                                                           std::span<const Migration> migrations);
[[nodiscard]] core::Result<std::vector<Migration>> load_migrations_from_directory(std::string_view directory);
[[nodiscard]] core::Result<MigrationReport> run_migrations_from_directory(Connection& connection,
                                                                          std::string_view directory);

// Compile-time-embedded migration assets — keep the canonical SQL files
// the binary ships with reachable without consulting the filesystem. The
// repositories below fall back to these when their `migrations_directory`
// option is empty; supplying an explicit directory still wins so tests
// can author one-off schemas under a tempdir.
[[nodiscard]] std::span<const Migration> built_in_audit_migrations();
[[nodiscard]] std::span<const Migration> built_in_session_migrations();
[[nodiscard]] std::span<const Migration> built_in_trace_migrations();

}  // namespace orangutan::storage
