// bench/storage/scenarios/session_repository.cpp
//
// A-vs-B comparison: raw Pool + StatementCache SQL vs. SessionRepository for
// appending and loading one 64-message session batch.

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
#include <oran/core/role.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kRows = 64;
constexpr std::string_view kAgentKey = "bench-agent";

constexpr std::string_view kRawAppendSql = R"sql(
INSERT INTO session_messages(session_id, agent_key, sequence, role, content_json, metadata_json, created_at)
VALUES (
  ?, ?,
  (
    SELECT COALESCE(MAX(sequence), 0) + 1
    FROM session_messages
    WHERE session_id = ? AND agent_key = ?
  ),
  ?, ?, '{}',
  strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
)
)sql";

constexpr std::string_view kRawLoadCountSql = R"sql(
SELECT COUNT(*)
FROM session_messages
WHERE session_id = ? AND agent_key = ?
)sql";

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

void migrate(asio::io_context& io, storage::SessionRepository& repo) {
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

[[gnu::noinline]] int run_raw_pool_append_load(asio::io_context& io, storage::Pool& pool, std::int64_t batch_id) {
  int rows = 0;
  const auto session_id = std::format("raw-{}", batch_id);
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
            auto body = std::format(R"json({{"text":"raw-{}"}})json", i);
            if (!cached->statement().bind_text(1, session_id) || !cached->statement().bind_text(2, kAgentKey) ||
                !cached->statement().bind_text(3, session_id) || !cached->statement().bind_text(4, kAgentKey) ||
                !cached->statement().bind_text(5, i % 2 == 0 ? "user" : "assistant") ||
                !cached->statement().bind_text(6, body)) {
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
        auto cached = reader->statement_cache().acquire(reader->connection(), kRawLoadCountSql);
        if (!cached) {
          std::abort();
        }
        if (!cached->statement().bind_text(1, session_id) || !cached->statement().bind_text(2, kAgentKey)) {
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
run_repository_append_load(asio::io_context& io, storage::SessionRepository& repo, std::int64_t batch_id) {
  int rows = 0;
  const auto session_id = std::format("repo-{}", batch_id);
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kRows; ++i) {
          auto appended = co_await repo.append_message(storage::AppendSessionMessageRequest{
              .session_id = session_id,
              .agent_key = std::string{kAgentKey},
              .role = i % 2 == 0 ? core::Role::user : core::Role::assistant,
              .content_json = std::format(R"json({{"text":"repo-{}"}})json", i),
          });
          if (!appended) {
            std::abort();
          }
        }
        auto loaded = co_await repo.load_messages(
            storage::SessionKey{.session_id = session_id, .agent_key = std::string{kAgentKey}});
        if (!loaded) {
          std::abort();
        }
        rows = static_cast<int>(loaded->size());
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

void register_session_repository(ankerl::nanobench::Bench& bench) {
  TempDb db{"session-repository"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 16});
  if (!pool) {
    std::abort();
  }
  storage::SessionRepository repo{*pool};
  migrate(io, repo);

  std::int64_t raw_batch = 0;
  std::int64_t repo_batch = 0;
  bench.run("storage.session_raw_pool_append_load", [&] {
    auto rows = run_raw_pool_append_load(io, *pool, raw_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.session_repository_append_load", [&] {
    auto rows = run_repository_append_load(io, repo, repo_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
