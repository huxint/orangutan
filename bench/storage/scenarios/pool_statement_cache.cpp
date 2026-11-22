// bench/storage/scenarios/pool_statement_cache.cpp
//
// A-vs-B comparison: prepare an INSERT through a writer Pool lease on every
// row vs. acquire the same INSERT through the writer slot's StatementCache.

#include <nanobench.h>

#include <chrono>
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
constexpr std::string_view kInsertSql = "INSERT INTO events(body) VALUES (?)";

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

void seed_schema(const std::string& path) {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = path});
  if (!connection) {
    std::abort();
  }
  if (!connection->execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)")) {
    std::abort();
  }
}

[[gnu::noinline]] int insert_with_pool_fresh_prepare(storage::WriterLease& lease) {
  if (!lease.connection().execute("DELETE FROM events")) {
    std::abort();
  }
  for (int i = 0; i < kRows; ++i) {
    auto prepared = lease.connection().prepare(kInsertSql);
    if (!prepared) {
      std::abort();
    }
    auto body = std::format("event-{}", i);
    if (!prepared->bind_text(1, body)) {
      std::abort();
    }
    auto step = prepared->step();
    if (!step || *step != storage::StepResult::done) {
      std::abort();
    }
  }
  return kRows;
}

[[gnu::noinline]] int insert_with_pool_cached_prepare(storage::WriterLease& lease) {
  if (!lease.connection().execute("DELETE FROM events")) {
    std::abort();
  }
  for (int i = 0; i < kRows; ++i) {
    auto cached = lease.statement_cache().acquire(lease.connection(), kInsertSql);
    if (!cached) {
      std::abort();
    }
    auto body = std::format("event-{}", i);
    if (!cached->statement().bind_text(1, body)) {
      std::abort();
    }
    auto step = cached->statement().step();
    if (!step || *step != storage::StepResult::done) {
      std::abort();
    }
  }
  return kRows;
}

template <typename InsertFn>
int run_with_writer(asio::io_context& io, storage::Pool& pool, InsertFn insert) {
  int rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto lease = co_await pool.acquire_writer();
        if (!lease) {
          std::abort();
        }
        rows = insert(*lease);
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return rows;
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

}  // namespace

void register_pool_statement_cache(ankerl::nanobench::Bench& bench) {
  TempDb db{"pool-statement-cache"};
  seed_schema(db.path());

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 1, .statement_cache_capacity = 4});
  if (!pool) {
    std::abort();
  }

  bench.run("storage.pool_fresh_prepare_insert", [&] {
    auto rows = run_with_writer(io, *pool, insert_with_pool_fresh_prepare);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.pool_cached_prepare_insert", [&] {
    auto rows = run_with_writer(io, *pool, insert_with_pool_cached_prepare);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
