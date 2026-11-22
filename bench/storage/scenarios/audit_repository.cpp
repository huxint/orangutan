// bench/storage/scenarios/audit_repository.cpp
//
// A-vs-B comparison: raw Pool + StatementCache SQL vs. AuditRepository for
// appending and listing one batch of audit events. The same shape as the
// session_repository bucket so operators can compare apples-to-apples.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kRows = 64;
constexpr std::string_view kAgentKey = "bench-agent";
constexpr std::string_view kIdentity = "bench-identity";

constexpr std::string_view kRawAppendSql = R"sql(
INSERT INTO audit_events(
  scope_key, agent_key, tool_name, identity, verdict, outcome, reason,
  input_hash_hex, metadata_json, created_at
)
VALUES (?, ?, ?, ?, ?, ?, ?, NULL, '{}', strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
)sql";

constexpr std::string_view kRawCountSql = "SELECT COUNT(*) FROM audit_events WHERE scope_key = ?";

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

class TempDb {
public:
  explicit TempDb(std::string_view tag) : path_{make_temp_path(tag)} {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

private:
  std::string path_;
};

void migrate(asio::io_context& io, storage::AuditRepository& repo) {
  bool migrated = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto report = co_await repo.migrate();
        if (!report) {
          std::abort();
        }
        migrated = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (!migrated) {
    std::abort();
  }
}

[[gnu::noinline]] int run_raw_pool_append_list(asio::io_context& io, storage::Pool& pool, std::int64_t batch_id) {
  int rows = 0;
  const auto scope = std::format("raw-{}", batch_id);
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        {
          auto writer = co_await pool.acquire_writer();
          if (!writer) {
            std::abort();
          }
          for (int i = 0; i < kRows; ++i) {
            auto cached = writer->statement_cache().acquire(writer->connection(), kRawAppendSql);
            if (!cached) {
              std::abort();
            }
            const auto reason = std::format("rule #{} (allow)", i);
            if (!cached->statement().bind_text(1, scope) || !cached->statement().bind_text(2, kAgentKey) ||
                !cached->statement().bind_text(3, "FileRead") || !cached->statement().bind_text(4, kIdentity) ||
                !cached->statement().bind_text(5, "allow") || !cached->statement().bind_text(6, "allow") ||
                !cached->statement().bind_text(7, reason)) {
              std::abort();
            }
            auto step = cached->statement().step();
            if (!step || *step != storage::StepResult::done) {
              std::abort();
            }
          }
        }

        auto reader = co_await pool.acquire_reader();
        if (!reader) {
          std::abort();
        }
        auto cached = reader->statement_cache().acquire(reader->connection(), kRawCountSql);
        if (!cached) {
          std::abort();
        }
        if (!cached->statement().bind_text(1, scope)) {
          std::abort();
        }
        auto step = cached->statement().step();
        if (!step || *step != storage::StepResult::row) {
          std::abort();
        }
        auto count = cached->statement().column_int64(0);
        if (!count) {
          std::abort();
        }
        rows = static_cast<int>(*count);
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kRows) {
    std::abort();
  }
  return rows;
}

[[gnu::noinline]] int
run_repository_append_list(asio::io_context& io, storage::AuditRepository& repo, std::int64_t batch_id) {
  int rows = 0;
  const auto scope = std::format("repo-{}", batch_id);
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kRows; ++i) {
          auto request = storage::AppendAuditEventRequest{
              .scope_key = scope,
              .agent_key = std::string{kAgentKey},
              .tool_name = "FileRead",
              .identity = std::string{kIdentity},
              .verdict = "allow",
              .outcome = "allow",
              .reason = std::format("rule #{} (allow)", i),
          };
          auto appended = co_await repo.append_event(std::move(request));
          if (!appended) {
            std::abort();
          }
        }
        auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = scope, .limit = 200});
        if (!listed) {
          std::abort();
        }
        rows = static_cast<int>(listed->size());
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kRows) {
    std::abort();
  }
  return rows;
}

}  // namespace

void register_audit_repository(ankerl::nanobench::Bench& bench) {
  TempDb db{"audit-repository"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 16});
  if (!pool) {
    std::abort();
  }
  storage::AuditRepository repo{*pool};
  migrate(io, repo);

  std::int64_t raw_batch = 0;
  std::int64_t repo_batch = 0;
  bench.run("storage.audit_raw_pool_append_list", [&] {
    auto rows = run_raw_pool_append_list(io, *pool, raw_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.audit_repository_append_list", [&] {
    auto rows = run_repository_append_list(io, repo, repo_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
