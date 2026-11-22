// bench/storage/scenarios/statement_cache.cpp
//
// A-vs-B comparison: re-prepare a statement on every insert vs. acquire it
// from a StatementCache. The cached path runs Connection::prepare exactly
// once per cache lifetime, while the fresh path pays the prepare cost for
// every row. Both paths bind and step the same insert and run on the same
// in-memory schema so only the prepare-vs-cache difference is measured.

#include <nanobench.h>

#include <cstdlib>
#include <format>
#include <utility>

#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kRows = 64;
constexpr std::string_view kInsertSql = "INSERT INTO events(id, body) VALUES (?, ?)";

storage::Connection make_connection() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  if (!connection) {
    std::abort();
  }
  if (!connection->execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)")) {
    std::abort();
  }
  return std::move(*connection);
}

[[gnu::noinline]] int insert_with_fresh_prepare(storage::Connection& connection) {
  for (int i = 0; i < kRows; ++i) {
    auto prepared = connection.prepare(kInsertSql);
    if (!prepared) {
      std::abort();
    }
    auto body = std::format("event-{}", i);
    if (!prepared->bind_int64(1, i) || !prepared->bind_text(2, body)) {
      std::abort();
    }
    auto step = prepared->step();
    if (!step || *step != storage::StepResult::done) {
      std::abort();
    }
  }
  return kRows;
}

[[gnu::noinline]] int insert_with_cached_prepare(storage::Connection& connection, storage::StatementCache& cache) {
  for (int i = 0; i < kRows; ++i) {
    auto lease = cache.acquire(connection, kInsertSql);
    if (!lease) {
      std::abort();
    }
    auto body = std::format("event-{}", i);
    if (!lease->statement().bind_int64(1, i) || !lease->statement().bind_text(2, body)) {
      std::abort();
    }
    auto step = lease->statement().step();
    if (!step || *step != storage::StepResult::done) {
      std::abort();
    }
  }
  return kRows;
}

}  // namespace

void register_statement_cache(ankerl::nanobench::Bench& bench) {
  bench.run("storage.fresh_prepare_insert", [] {
    auto connection = make_connection();
    auto rows = insert_with_fresh_prepare(connection);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.cached_prepare_insert", [] {
    auto connection = make_connection();
    auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 4});
    if (!cache) {
      std::abort();
    }
    auto rows = insert_with_cached_prepare(connection, *cache);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
