// tests/storage/test_pool.cpp — async connection pool coverage.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

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

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("Pool::open rejects invalid options", "[unit][storage][pool]") {
  asio::io_context io;

  auto empty_path = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = ""});
  REQUIRE_FALSE(empty_path.has_value());
  REQUIRE(empty_path.error().kind() == core::ErrorKind::invalid_argument);

  TempDb db{"oran-pool-zero"};
  auto zero_readers =
      storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 0});
  REQUIRE_FALSE(zero_readers.has_value());
  REQUIRE(zero_readers.error().kind() == core::ErrorKind::invalid_argument);

  auto zero_cache =
      storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .statement_cache_capacity = 0});
  REQUIRE_FALSE(zero_cache.has_value());
  REQUIRE(zero_cache.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("Pool::open creates writer and N readers", "[unit][storage][pool]") {
  TempDb db{"oran-pool-open"};
  asio::io_context io;

  auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 3});
  REQUIRE(pool.has_value());
  REQUIRE(pool->valid());
  REQUIRE(pool->reader_count() == 3);
  REQUIRE(pool->readers_available() == 3);
  REQUIRE_FALSE(pool->writer_busy());
}

TEST_CASE("Pool leases resume on the requesting strand", "[unit][storage][pool][execution]") {
  TempDb db{"oran-pool-caller-strand"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string()});
    REQUIRE(pool.has_value());
    auto caller = asio::make_strand(io);

    co_await asio::co_spawn(
        caller,
        [&]() -> async::Awaitable<void> {
          auto writer = co_await pool->acquire_writer();
          REQUIRE(writer.has_value());
          CHECK(caller.running_in_this_thread());
          writer->release();

          auto reader = co_await pool->acquire_reader();
          REQUIRE(reader.has_value());
          CHECK(caller.running_in_this_thread());
        },
        asio::use_awaitable);
  });
}

TEST_CASE("Pool writer acquire is exclusive and serializes migrations", "[unit][storage][pool]") {
  TempDb db{"oran-pool-writer"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 2});
    REQUIRE(pool.has_value());

    auto first = co_await pool->acquire_writer();
    REQUIRE(first.has_value());
    REQUIRE(first->valid());
    REQUIRE(pool->writer_busy());

    auto migrations = std::vector<storage::Migration>{
        storage::Migration{.version = 1, .name = "create", .sql = "CREATE TABLE notes(id INTEGER PRIMARY KEY)"},
    };
    auto report = storage::run_migrations(first->connection(), migrations);
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);

    bool second_completed = false;
    std::optional<storage::WriterLease> second_lease;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          auto second = co_await pool->acquire_writer();
          REQUIRE(second.has_value());
          second_lease = std::move(*second);
          second_completed = true;
          co_return;
        },
        asio::detached);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(second_completed);
    REQUIRE(pool->writer_busy());

    first->release();
    co_await asio::post(io, asio::use_awaitable);

    REQUIRE(second_completed);
    REQUIRE(second_lease.has_value());
    REQUIRE(pool->writer_busy());

    second_lease->release();
    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(pool->writer_busy());
  });
}

TEST_CASE("Pool readers acquire distinct slots and wait when exhausted", "[unit][storage][pool]") {
  TempDb db{"oran-pool-readers"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 2});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      auto migrations = std::vector<storage::Migration>{
          storage::Migration{.version = 1, .name = "schema", .sql = "CREATE TABLE values_(id INTEGER PRIMARY KEY)"},
      };
      auto report = storage::run_migrations(writer->connection(), migrations);
      REQUIRE(report.has_value());
    }

    auto first = co_await pool->acquire_reader();
    auto second = co_await pool->acquire_reader();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->valid());
    REQUIRE(second->valid());
    REQUIRE(first->slot() != second->slot());
    REQUIRE(pool->readers_available() == 0);

    auto first_query = first->connection().query("SELECT COUNT(*) FROM values_");
    auto second_query = second->connection().query("SELECT COUNT(*) FROM values_");
    REQUIRE(first_query.has_value());
    REQUIRE(second_query.has_value());

    bool third_completed = false;
    std::size_t third_slot = 99;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          auto third = co_await pool->acquire_reader();
          REQUIRE(third.has_value());
          third_slot = third->slot();
          third_completed = true;
          co_return;
        },
        asio::detached);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(third_completed);

    const auto first_slot = first->slot();
    first->release();
    co_await asio::post(io, asio::use_awaitable);

    REQUIRE(third_completed);
    REQUIRE(third_slot == first_slot);
  });
}

TEST_CASE("Pool waiters resume in FIFO order", "[unit][storage][pool]") {
  TempDb db{"oran-pool-fifo"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
    REQUIRE(pool.has_value());

    auto held = co_await pool->acquire_reader();
    REQUIRE(held.has_value());

    std::vector<int> arrivals;
    auto enqueue = [&](int label) {
      asio::co_spawn(
          io,
          [&, label]() -> async::Awaitable<void> {
            auto lease = co_await pool->acquire_reader();
            REQUIRE(lease.has_value());
            arrivals.push_back(label);
            co_return;
          },
          asio::detached);
    };

    enqueue(1);
    enqueue(2);
    enqueue(3);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE(arrivals.empty());

    held->release();
    for (int i = 0; i < 6; ++i) {
      co_await asio::post(io, asio::use_awaitable);
    }

    REQUIRE(arrivals == std::vector<int>{1, 2, 3});
  });
}

TEST_CASE("Pool reader writes are rejected by SQLite", "[unit][storage][pool]") {
  TempDb db{"oran-pool-readonly"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      auto migrations = std::vector<storage::Migration>{
          storage::Migration{.version = 1, .name = "schema", .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY)"},
      };
      auto report = storage::run_migrations(writer->connection(), migrations);
      REQUIRE(report.has_value());
    }

    auto reader = co_await pool->acquire_reader();
    REQUIRE(reader.has_value());

    auto write = reader->connection().execute("INSERT INTO items(id) VALUES (1)");
    REQUIRE_FALSE(write.has_value());
    REQUIRE(write.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("Pool writes by writer are visible to readers", "[unit][storage][pool]") {
  TempDb db{"oran-pool-rw"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 2});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      REQUIRE(writer->connection().execute("CREATE TABLE words(value TEXT)").has_value());
      REQUIRE(writer->connection().execute("INSERT INTO words(value) VALUES ('alpha'), ('beta')").has_value());
    }

    auto reader = co_await pool->acquire_reader();
    REQUIRE(reader.has_value());
    auto rows = reader->connection().query("SELECT value FROM words ORDER BY value");
    REQUIRE(rows.has_value());
    REQUIRE(rows->rows.size() == 2);
    REQUIRE(rows->rows[0].values[0] == "alpha");
    REQUIRE(rows->rows[1].values[0] == "beta");
  });
}

TEST_CASE("Pool writer lease exposes a cache that persists across leases", "[unit][storage][pool]") {
  TempDb db{"oran-pool-writer-cache"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 1, .statement_cache_capacity = 2});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      REQUIRE(writer->statement_cache().valid());
      REQUIRE(writer->connection().execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)").has_value());

      {
        auto insert = writer->statement_cache().acquire(writer->connection(), "INSERT INTO events(body) VALUES (?)");
        REQUIRE(insert.has_value());
        REQUIRE(insert->statement().bind_text(1, "alpha").has_value());
        auto step = insert->statement().step();
        REQUIRE(step.has_value());
        REQUIRE(*step == storage::StepResult::done);
      }

      REQUIRE(writer->statement_cache().misses() == 1);
      REQUIRE(writer->statement_cache().hits() == 0);
    }

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());

      {
        auto insert = writer->statement_cache().acquire(writer->connection(), "INSERT INTO events(body) VALUES (?)");
        REQUIRE(insert.has_value());
        REQUIRE(insert->statement().bind_text(1, "beta").has_value());
        auto step = insert->statement().step();
        REQUIRE(step.has_value());
        REQUIRE(*step == storage::StepResult::done);
      }

      REQUIRE(writer->statement_cache().misses() == 1);
      REQUIRE(writer->statement_cache().hits() == 1);

      auto rows = writer->connection().query("SELECT body FROM events ORDER BY id");
      REQUIRE(rows.has_value());
      REQUIRE(rows->rows.size() == 2);
      REQUIRE(rows->rows[0].values[0] == "alpha");
      REQUIRE(rows->rows[1].values[0] == "beta");
    }
  });
}

TEST_CASE("Pool reader lease exposes a slot cache that persists across releases", "[unit][storage][pool]") {
  TempDb db{"oran-pool-reader-cache"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 1, .statement_cache_capacity = 2});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      REQUIRE(writer->connection().execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)").has_value());
      REQUIRE(writer->connection().execute("INSERT INTO events(body) VALUES ('alpha'), ('beta')").has_value());
    }

    {
      auto reader = co_await pool->acquire_reader();
      REQUIRE(reader.has_value());
      REQUIRE(reader->statement_cache().valid());

      auto count = reader->statement_cache().acquire(reader->connection(), "SELECT COUNT(*) FROM events");
      REQUIRE(count.has_value());
      auto step = count->statement().step();
      REQUIRE(step.has_value());
      REQUIRE(*step == storage::StepResult::row);
      auto value = count->statement().column_int64(0);
      REQUIRE(value.has_value());
      REQUIRE(*value == 2);
      REQUIRE(reader->statement_cache().misses() == 1);
      REQUIRE(reader->statement_cache().hits() == 0);
    }

    {
      auto reader = co_await pool->acquire_reader();
      REQUIRE(reader.has_value());
      auto count = reader->statement_cache().acquire(reader->connection(), "SELECT COUNT(*) FROM events");
      REQUIRE(count.has_value());
      auto step = count->statement().step();
      REQUIRE(step.has_value());
      REQUIRE(*step == storage::StepResult::row);
      auto value = count->statement().column_int64(0);
      REQUIRE(value.has_value());
      REQUIRE(*value == 2);
      REQUIRE(reader->statement_cache().misses() == 1);
      REQUIRE(reader->statement_cache().hits() == 1);
    }
  });
}

TEST_CASE("Pool reader statement caches are isolated per slot", "[unit][storage][pool]") {
  TempDb db{"oran-pool-reader-cache-slots"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 2});
    REQUIRE(pool.has_value());

    {
      auto writer = co_await pool->acquire_writer();
      REQUIRE(writer.has_value());
      REQUIRE(writer->connection().execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)").has_value());
      REQUIRE(writer->connection().execute("INSERT INTO events(body) VALUES ('alpha')").has_value());
    }

    auto first = co_await pool->acquire_reader();
    auto second = co_await pool->acquire_reader();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->slot() != second->slot());

    {
      auto first_count = first->statement_cache().acquire(first->connection(), "SELECT COUNT(*) FROM events");
      REQUIRE(first_count.has_value());
      REQUIRE(first_count->statement().step().has_value());
      auto second_count = second->statement_cache().acquire(second->connection(), "SELECT COUNT(*) FROM events");
      REQUIRE(second_count.has_value());
      REQUIRE(second_count->statement().step().has_value());
    }

    REQUIRE(first->statement_cache().misses() == 1);
    REQUIRE(first->statement_cache().hits() == 0);
    REQUIRE(second->statement_cache().misses() == 1);
    REQUIRE(second->statement_cache().hits() == 0);

    {
      auto first_again = first->statement_cache().acquire(first->connection(), "SELECT COUNT(*) FROM events");
      REQUIRE(first_again.has_value());
      REQUIRE(first_again->statement().step().has_value());
    }

    REQUIRE(first->statement_cache().misses() == 1);
    REQUIRE(first->statement_cache().hits() == 1);
    REQUIRE(second->statement_cache().misses() == 1);
    REQUIRE(second->statement_cache().hits() == 0);
  });
}

TEST_CASE("Pool reader acquire observes coroutine cancellation", "[unit][storage][pool]") {
  TempDb db{"oran-pool-cancel"};

  asio::io_context io;
  auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
  REQUIRE(pool.has_value());

  auto held_pool = std::make_shared<storage::Pool>(std::move(*pool));

  std::optional<storage::ReaderLease> held;
  bool primary_done = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto lease = co_await held_pool->acquire_reader();
        REQUIRE(lease.has_value());
        held = std::move(*lease);
        primary_done = true;
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  REQUIRE(primary_done);
  REQUIRE(held.has_value());

  asio::cancellation_signal signal;
  std::optional<core::Result<storage::ReaderLease>> waiter_result;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<storage::ReaderLease>> { co_return co_await held_pool->acquire_reader(); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<storage::ReaderLease> r) {
        REQUIRE_FALSE(ep);
        waiter_result = std::move(r);
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  REQUIRE(waiter_result.has_value());
  REQUIRE_FALSE(waiter_result->has_value());
  REQUIRE(waiter_result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(held_pool->readers_available() == 0);
}

TEST_CASE("Pool cancelled writer waiter cannot consume the released writer", "[unit][storage][pool]") {
  TempDb db{"oran-pool-cancel-writer"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
    REQUIRE(pool.has_value());
    auto held = co_await pool->acquire_writer();
    REQUIRE(held.has_value());

    asio::cancellation_signal signal;
    std::optional<core::Result<storage::WriterLease>> cancelled_result;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<storage::WriterLease>> { co_return co_await pool->acquire_writer(); },
        asio::bind_cancellation_slot(signal.slot(),
                                     [&](std::exception_ptr ep, core::Result<storage::WriterLease> result) {
                                       REQUIRE_FALSE(ep);
                                       cancelled_result = std::move(result);
                                     }));
    co_await asio::post(io, asio::use_awaitable);

    held->release();
    signal.emit(asio::cancellation_type::terminal);
    for (int i = 0; i < 4; ++i) {
      co_await asio::post(io, asio::use_awaitable);
    }

    REQUIRE(cancelled_result.has_value());
    REQUIRE_FALSE(cancelled_result->has_value());
    REQUIRE(cancelled_result->error().kind() == core::ErrorKind::cancelled);

    auto next = co_await pool->acquire_writer();
    REQUIRE(next.has_value());
    REQUIRE(next->valid());
    next->release();
    REQUIRE_FALSE(pool->writer_busy());
  });
}

TEST_CASE("Pool cancelled reader waiter cannot consume the released reader", "[unit][storage][pool]") {
  TempDb db{"oran-pool-cancel-reader-release"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
    REQUIRE(pool.has_value());
    auto held = co_await pool->acquire_reader();
    REQUIRE(held.has_value());

    asio::cancellation_signal signal;
    std::optional<core::Result<storage::ReaderLease>> cancelled_result;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<storage::ReaderLease>> { co_return co_await pool->acquire_reader(); },
        asio::bind_cancellation_slot(signal.slot(),
                                     [&](std::exception_ptr ep, core::Result<storage::ReaderLease> result) {
                                       REQUIRE_FALSE(ep);
                                       cancelled_result = std::move(result);
                                     }));
    co_await asio::post(io, asio::use_awaitable);

    held->release();
    signal.emit(asio::cancellation_type::terminal);
    for (int i = 0; i < 4; ++i) {
      co_await asio::post(io, asio::use_awaitable);
    }

    REQUIRE(cancelled_result.has_value());
    REQUIRE_FALSE(cancelled_result->has_value());
    REQUIRE(cancelled_result->error().kind() == core::ErrorKind::cancelled);

    auto next = co_await pool->acquire_reader();
    REQUIRE(next.has_value());
    REQUIRE(next->valid());
    next->release();
    REQUIRE(pool->readers_available() == 1);
  });
}

TEST_CASE("Pool lease release is idempotent", "[unit][storage][pool]") {
  TempDb db{"oran-pool-release"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db.string(), .reader_count = 1});
    REQUIRE(pool.has_value());

    auto reader = co_await pool->acquire_reader();
    REQUIRE(reader.has_value());
    REQUIRE(pool->readers_available() == 0);
    reader->release();
    REQUIRE_FALSE(reader->valid());
    reader->release();
    REQUIRE(pool->readers_available() == 1);
  });
}
