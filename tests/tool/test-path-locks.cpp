#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/core/error.hpp>

#include "../../src/oran-tool/_impl/path-lock-table.hpp"
#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace test = orangutan::tests;
using orangutan::tool::detail::PathLockGuard;
using orangutan::tool::detail::PathLockMode;
using orangutan::tool::detail::PathLockTable;

namespace {

struct LockRequest {
  asio::cancellation_signal cancellation;
  std::optional<core::Result<PathLockGuard>> result;
};

std::shared_ptr<LockRequest> request(PathLockTable& table, asio::io_context& io, PathLockMode mode) {
  auto pending = std::make_shared<LockRequest>();
  asio::co_spawn(
      io,
      table.acquire(io.get_executor(), "/shared/path", mode),
      asio::bind_cancellation_slot(pending->cancellation.slot(),
                                   [pending](std::exception_ptr failure, core::Result<PathLockGuard> result) {
                                     REQUIRE_FALSE(failure);
                                     pending->result = std::move(result);
                                   }));
  return pending;
}

// No blocking wait: a missing handoff leaves an empty result for the assertion.
void run_ready(asio::io_context& io) {
  io.restart();
  io.poll();
}

}  // namespace

TEST_CASE("PathLockTable retains only live paths during churn", "[unit][tool][lock][ownership]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    PathLockTable table;
    auto held = co_await table.acquire(io.get_executor(), "/held/path", PathLockMode::exclusive);
    REQUIRE(held.has_value());
    for (std::size_t i = 0; i < 10'000; ++i) {
      {
        auto guard =
            co_await table.acquire(io.get_executor(), "/changing/" + std::to_string(i), PathLockMode::exclusive);
        REQUIRE(guard.has_value());
        REQUIRE(table.size() == 2);
      }
      REQUIRE(table.size() == 1);
    }
    *held = PathLockGuard{};
    REQUIRE(table.size() == 0);
  });
}

TEST_CASE("PathLockTable hands a path from shared holders through FIFO waiters", "[unit][tool][lock][ownership]") {
  PathLockTable table;
  asio::io_context io;
  auto first = request(table, io, PathLockMode::shared);
  auto second = request(table, io, PathLockMode::shared);
  run_ready(io);
  REQUIRE(first->result.has_value());
  REQUIRE(first->result->has_value());
  REQUIRE(second->result.has_value());
  REQUIRE(second->result->has_value());

  auto writer = request(table, io, PathLockMode::exclusive);
  auto reader = request(table, io, PathLockMode::shared);
  auto other_reader = request(table, io, PathLockMode::shared);
  run_ready(io);
  REQUIRE_FALSE(writer->result.has_value());
  REQUIRE_FALSE(reader->result.has_value());
  REQUIRE_FALSE(other_reader->result.has_value());

  first->result.reset();
  run_ready(io);
  REQUIRE_FALSE(writer->result.has_value());
  second->result.reset();
  REQUIRE(table.size() == 1);
  REQUIRE_FALSE(writer->result.has_value());

  auto last = request(table, io, PathLockMode::exclusive);
  run_ready(io);
  REQUIRE(writer->result.has_value());
  REQUIRE(writer->result->has_value());
  REQUIRE_FALSE(reader->result.has_value());
  REQUIRE_FALSE(last->result.has_value());

  writer->result.reset();
  run_ready(io);
  REQUIRE(reader->result.has_value());
  REQUIRE(reader->result->has_value());
  REQUIRE(other_reader->result.has_value());
  REQUIRE(other_reader->result->has_value());
  REQUIRE_FALSE(last->result.has_value());
  reader->result.reset();
  run_ready(io);
  REQUIRE_FALSE(last->result.has_value());
  other_reader->result.reset();
  run_ready(io);
  REQUIRE(last->result.has_value());
  REQUIRE(last->result->has_value());
  last->result.reset();
  REQUIRE(table.size() == 0);
}

TEST_CASE("PathLockTable cancelling a queued writer admits readers beside a live reader",
          "[unit][tool][lock][cancellation]") {
  PathLockTable table;
  asio::io_context io;
  auto holder = request(table, io, PathLockMode::shared);
  run_ready(io);
  REQUIRE(holder->result.has_value());
  REQUIRE(holder->result->has_value());

  auto writer = request(table, io, PathLockMode::exclusive);
  auto reader = request(table, io, PathLockMode::shared);
  run_ready(io);
  REQUIRE_FALSE(writer->result.has_value());
  REQUIRE_FALSE(reader->result.has_value());

  writer->cancellation.emit(asio::cancellation_type::all);
  run_ready(io);
  REQUIRE(writer->result.has_value());
  REQUIRE_FALSE(writer->result->has_value());
  REQUIRE(writer->result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(reader->result.has_value());
  REQUIRE(reader->result->has_value());
  holder->result.reset();
  REQUIRE(table.size() == 1);
  reader->result.reset();
  REQUIRE(table.size() == 0);
}

TEST_CASE("PathLockTable cancellation releases queued and reserved ownership",
          "[unit][tool][lock][cancellation][ownership]") {
  const auto mode = GENERATE(PathLockMode::shared, PathLockMode::exclusive);
  const auto grant_before_resume = GENERATE(false, true);
  const auto cancel_before_grant = GENERATE(false, true);
  PathLockTable table;
  asio::io_context io;
  auto holder = request(table, io, PathLockMode::exclusive);
  run_ready(io);
  REQUIRE(holder->result.has_value());
  REQUIRE(holder->result->has_value());
  auto cancelled = request(table, io, mode);
  run_ready(io);
  REQUIRE_FALSE(cancelled->result.has_value());

  if (cancel_before_grant) {
    cancelled->cancellation.emit(asio::cancellation_type::all);
  }
  if (grant_before_resume) {
    // Grant while the cancelled receive's completion is still queued.
    holder->result.reset();
  }
  if (!cancel_before_grant) {
    cancelled->cancellation.emit(asio::cancellation_type::all);
  }
  REQUIRE(table.size() == 1);
  run_ready(io);
  REQUIRE(cancelled->result.has_value());
  REQUIRE_FALSE(cancelled->result->has_value());
  REQUIRE(cancelled->result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(table.size() == (grant_before_resume ? 0 : 1));
  holder->result.reset();
  REQUIRE(table.size() == 0);

  auto recovered = request(table, io, PathLockMode::exclusive);
  run_ready(io);
  REQUIRE(recovered->result.has_value());
  REQUIRE(recovered->result->has_value());
  recovered->result.reset();
  REQUIRE(table.size() == 0);
}

TEST_CASE("PathLockTable returns a cancelled grant to the next waiter", "[unit][tool][lock][cancellation][ownership]") {
  const auto mode = GENERATE(PathLockMode::shared, PathLockMode::exclusive);
  PathLockTable table;
  asio::io_context io;
  auto holder = request(table, io, PathLockMode::exclusive);
  run_ready(io);
  REQUIRE(holder->result.has_value());
  REQUIRE(holder->result->has_value());
  auto cancelled = request(table, io, mode);
  auto successor = request(table, io, PathLockMode::exclusive);
  run_ready(io);
  REQUIRE_FALSE(cancelled->result.has_value());
  REQUIRE_FALSE(successor->result.has_value());

  cancelled->cancellation.emit(asio::cancellation_type::all);
  holder->result.reset();
  run_ready(io);
  REQUIRE(cancelled->result.has_value());
  REQUIRE_FALSE(cancelled->result->has_value());
  REQUIRE(cancelled->result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(successor->result.has_value());
  REQUIRE(successor->result->has_value());
  successor->result.reset();
  REQUIRE(table.size() == 0);
}
