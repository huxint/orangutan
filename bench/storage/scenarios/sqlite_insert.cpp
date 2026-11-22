// bench/storage/scenarios/sqlite_insert.cpp
//
// A-vs-B comparison: literal execute inserts vs. a reused prepared statement.

#include <nanobench.h>

#include <cstdlib>
#include <format>
#include <string>

#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr int kRows = 64;

storage::Connection make_connection() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  if (!connection) {
    std::abort();
  }
  auto created = connection->execute("CREATE TABLE events(id INTEGER PRIMARY KEY, body TEXT)");
  if (!created) {
    std::abort();
  }
  return std::move(*connection);
}

[[gnu::noinline]] int insert_with_execute() {
  auto connection = make_connection();
  for (int i = 0; i < kRows; ++i) {
    auto sql = std::format("INSERT INTO events(id, body) VALUES ({}, 'event-{}')", i, i);
    auto inserted = connection.execute(sql);
    if (!inserted) {
      std::abort();
    }
  }
  return kRows;
}

[[gnu::noinline]] int insert_with_prepared_statement() {
  auto connection = make_connection();
  auto insert = connection.prepare("INSERT INTO events(id, body) VALUES (?, ?)");
  if (!insert) {
    std::abort();
  }

  for (int i = 0; i < kRows; ++i) {
    auto body = std::format("event-{}", i);
    if (!insert->bind_int64(1, i)) {
      std::abort();
    }
    if (!insert->bind_text(2, body)) {
      std::abort();
    }
    auto step = insert->step();
    if (!step || *step != storage::StepResult::done) {
      std::abort();
    }
    if (!insert->reset()) {
      std::abort();
    }
    if (!insert->clear_bindings()) {
      std::abort();
    }
  }
  return kRows;
}

}  // namespace

void register_sqlite_insert(ankerl::nanobench::Bench& bench) {
  bench.run("storage.execute_literal_inserts", [] {
    auto rows = insert_with_execute();
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.prepared_statement_inserts", [] {
    auto rows = insert_with_prepared_statement();
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
