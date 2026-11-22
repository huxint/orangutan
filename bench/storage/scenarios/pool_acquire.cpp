// bench/storage/scenarios/pool_acquire.cpp
//
// A-vs-B comparison: direct Connection re-use vs. async Pool acquire + release
// for an identical SELECT workload. Both paths run on the same in-memory schema
// so only the acquisition overhead differs.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr std::size_t kReaderCount = 4;
constexpr int kBatchQueries = 16;
constexpr std::size_t kContentionAcquireCount = 32;

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

void seed(const std::string& path) {
  auto writer = storage::Connection::open(storage::ConnectionOptions{.path = path});
  if (!writer) {
    std::abort();
  }
  if (!writer->execute("CREATE TABLE IF NOT EXISTS rows_(id INTEGER PRIMARY KEY)")) {
    std::abort();
  }
  for (int i = 0; i < 32; ++i) {
    auto insert = writer->execute("INSERT INTO rows_(id) VALUES (NULL)");
    if (!insert) {
      std::abort();
    }
  }
}

[[gnu::noinline]] std::int64_t run_direct(storage::Connection& reader) {
  std::int64_t accum = 0;
  for (int i = 0; i < kBatchQueries; ++i) {
    auto result = reader.query("SELECT COUNT(*) FROM rows_");
    if (!result || result->rows.empty() || !result->rows.front().values.front()) {
      std::abort();
    }
    accum += std::stoll(*result->rows.front().values.front());
  }
  return accum;
}

[[gnu::noinline]] std::int64_t run_pool(asio::io_context& io, storage::Pool& pool) {
  std::int64_t accum = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kBatchQueries; ++i) {
          auto lease = co_await pool.acquire_reader();
          if (!lease) {
            std::abort();
          }
          auto result = lease->connection().query("SELECT COUNT(*) FROM rows_");
          if (!result || result->rows.empty() || !result->rows.front().values.front()) {
            std::abort();
          }
          accum += std::stoll(*result->rows.front().values.front());
        }
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return accum;
}

[[gnu::noinline]] std::uint64_t run_pool_reader_uncontended_batch(asio::io_context& io, storage::Pool& pool) {
  std::uint64_t checksum = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (std::size_t i = 0; i < kContentionAcquireCount; ++i) {
          auto lease = co_await pool.acquire_reader();
          if (!lease) {
            std::abort();
          }
          checksum += static_cast<std::uint64_t>(lease->slot() + 1U);
          lease->release();
        }
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return checksum;
}

[[gnu::noinline]] std::uint64_t run_pool_reader_contended_batch(asio::io_context& io, storage::Pool& pool) {
  std::uint64_t checksum = 0;
  std::size_t started = 0;
  std::size_t completed = 0;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto held = co_await pool.acquire_reader();
        if (!held) {
          std::abort();
        }

        for (std::size_t i = 0; i < kContentionAcquireCount; ++i) {
          asio::co_spawn(
              io,
              [&, i]() -> async::Awaitable<void> {
                ++started;
                auto lease = co_await pool.acquire_reader();
                if (!lease) {
                  std::abort();
                }
                checksum += static_cast<std::uint64_t>(i + 1U) * static_cast<std::uint64_t>(lease->slot() + 1U);
                ++completed;
                lease->release();
                co_return;
              },
              asio::detached);
        }

        while (started < kContentionAcquireCount) {
          co_await asio::post(io, asio::use_awaitable);
        }

        held->release();

        while (completed < kContentionAcquireCount) {
          co_await asio::post(io, asio::use_awaitable);
        }

        co_return;
      },
      asio::detached);

  io.restart();
  io.run();
  if (completed != kContentionAcquireCount) {
    std::abort();
  }
  return checksum;
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

void register_pool_acquire(ankerl::nanobench::Bench& bench) {
  TempDb db{"pool"};
  seed(db.path());

  auto reader = storage::Connection::open(storage::ConnectionOptions{
      .path = db.path(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
  });
  if (!reader) {
    std::abort();
  }

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.path(), .reader_count = kReaderCount});
  if (!pool) {
    std::abort();
  }

  bench.run("storage.direct_connection_query", [&] {
    auto result = run_direct(*reader);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.pool_acquire_query", [&] {
    auto result = run_pool(io, *pool);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  auto contention_pool =
      storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.path(), .reader_count = 1});
  if (!contention_pool) {
    std::abort();
  }

  bench.run("storage.pool_reader_uncontended_acquire_batch", [&] {
    auto result = run_pool_reader_uncontended_batch(io, *contention_pool);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
  bench.run("storage.pool_reader_contended_fifo_acquire_batch", [&] {
    auto result = run_pool_reader_contended_batch(io, *contention_pool);
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

}  // namespace orangutan::bench
