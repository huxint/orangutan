// tests/storage/test_statement_cache.cpp — prepared-statement cache coverage.

#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <oran/storage.hpp>

namespace core = orangutan::core;
namespace storage = orangutan::storage;

namespace {

storage::Connection open_memory() {
  auto connection = storage::Connection::open(storage::ConnectionOptions{.path = ":memory:", .enable_wal = false});
  REQUIRE(connection.has_value());
  return std::move(*connection);
}

void seed_schema(storage::Connection& connection) {
  REQUIRE(connection.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, label TEXT)").has_value());
}

}  // namespace

TEST_CASE("StatementCache::open rejects zero capacity", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 0});
  REQUIRE_FALSE(cache.has_value());
  REQUIRE(cache.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("StatementCache::open creates a valid empty cache", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 4});
  REQUIRE(cache.has_value());
  REQUIRE(cache->valid());
  REQUIRE(cache->capacity() == 4);
  REQUIRE(cache->size() == 0);
  REQUIRE(cache->hits() == 0);
  REQUIRE(cache->misses() == 0);
  REQUIRE(cache->evictions() == 0);
}

TEST_CASE("default-constructed StatementCache reports invalid and rejects acquire",
          "[unit][storage][statement_cache]") {
  storage::StatementCache cache;
  REQUIRE_FALSE(cache.valid());

  auto connection = open_memory();
  auto result = cache.acquire(connection, "SELECT 1");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("acquire rejects empty SQL", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto connection = open_memory();
  auto result = cache.acquire(connection, "");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("acquire returns a miss on first call and a hit on the second", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 4}).value();
  auto connection = open_memory();
  seed_schema(connection);

  {
    auto first = cache.acquire(connection, "INSERT INTO items(id, label) VALUES (?, ?)");
    REQUIRE(first.has_value());
    REQUIRE(first->valid());
    REQUIRE(first->statement().bind_int64(1, 1).has_value());
    REQUIRE(first->statement().bind_text(2, "alpha").has_value());
    auto step = first->statement().step();
    REQUIRE(step.has_value());
    REQUIRE(*step == storage::StepResult::done);
  }

  REQUIRE(cache.size() == 1);
  REQUIRE(cache.misses() == 1);
  REQUIRE(cache.hits() == 0);

  {
    auto second = cache.acquire(connection, "INSERT INTO items(id, label) VALUES (?, ?)");
    REQUIRE(second.has_value());
    REQUIRE(second->statement().bind_int64(1, 2).has_value());
    REQUIRE(second->statement().bind_text(2, "beta").has_value());
    auto step = second->statement().step();
    REQUIRE(step.has_value());
    REQUIRE(*step == storage::StepResult::done);
  }

  REQUIRE(cache.size() == 1);
  REQUIRE(cache.misses() == 1);
  REQUIRE(cache.hits() == 1);

  auto rows = connection.query("SELECT label FROM items ORDER BY id");
  REQUIRE(rows.has_value());
  REQUIRE(rows->rows.size() == 2);
  REQUIRE(rows->rows[0].values[0] == "alpha");
  REQUIRE(rows->rows[1].values[0] == "beta");
}

TEST_CASE("releasing a lease resets the underlying statement state", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto connection = open_memory();
  seed_schema(connection);

  {
    auto lease = cache.acquire(connection, "INSERT INTO items(id, label) VALUES (?, ?)");
    REQUIRE(lease.has_value());
    REQUIRE(lease->statement().bind_int64(1, 7).has_value());
    REQUIRE(lease->statement().bind_text(2, "seven").has_value());
    REQUIRE(lease->statement().step().has_value());
  }

  auto reuse = cache.acquire(connection, "INSERT INTO items(id, label) VALUES (?, ?)");
  REQUIRE(reuse.has_value());
  REQUIRE(reuse->statement().bind_int64(1, 8).has_value());
  REQUIRE(reuse->statement().bind_text(2, "eight").has_value());
  auto step = reuse->statement().step();
  REQUIRE(step.has_value());
  REQUIRE(*step == storage::StepResult::done);
}

TEST_CASE("acquiring the same SQL while leased returns conflict", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto connection = open_memory();
  seed_schema(connection);

  auto held = cache.acquire(connection, "SELECT COUNT(*) FROM items");
  REQUIRE(held.has_value());

  auto duplicate = cache.acquire(connection, "SELECT COUNT(*) FROM items");
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("cache evicts the least-recently-used unleased entry at capacity", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto connection = open_memory();
  seed_schema(connection);

  {
    auto a = cache.acquire(connection, "SELECT 1");
    REQUIRE(a.has_value());
  }
  {
    auto b = cache.acquire(connection, "SELECT 2");
    REQUIRE(b.has_value());
  }
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.evictions() == 0);

  // Touch SELECT 1 to promote it to most-recently-used; SELECT 2 is now the LRU.
  {
    auto a_again = cache.acquire(connection, "SELECT 1");
    REQUIRE(a_again.has_value());
  }
  REQUIRE(cache.hits() == 1);
  REQUIRE(cache.size() == 2);

  {
    auto c = cache.acquire(connection, "SELECT 3");
    REQUIRE(c.has_value());
  }
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.evictions() == 1);

  // SELECT 2 should be the evicted one: re-acquiring it must be a miss.
  {
    auto b_again = cache.acquire(connection, "SELECT 2");
    REQUIRE(b_again.has_value());
  }
  REQUIRE(cache.evictions() == 2);
  REQUIRE(cache.misses() == 4);  // 3 inserts + 1 re-miss
  REQUIRE(cache.hits() == 1);
}

TEST_CASE("when every entry is leased a new miss returns a transient lease", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 1}).value();
  auto connection = open_memory();
  seed_schema(connection);

  auto first = cache.acquire(connection, "SELECT 1");
  REQUIRE(first.has_value());
  REQUIRE(cache.size() == 1);

  auto transient = cache.acquire(connection, "SELECT 2");
  REQUIRE(transient.has_value());
  REQUIRE(transient->valid());
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.evictions() == 0);

  transient->release();
  REQUIRE(cache.size() == 1);

  // SELECT 2 should not be in the cache; acquiring it again is still a miss.
  first->release();
  auto second = cache.acquire(connection, "SELECT 2");
  REQUIRE(second.has_value());
  REQUIRE(cache.misses() == 3);
}

TEST_CASE("clear() drops unleased entries and orphans leased ones", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 4}).value();
  auto connection = open_memory();
  seed_schema(connection);

  auto held = cache.acquire(connection, "SELECT 1");
  REQUIRE(held.has_value());
  {
    auto a = cache.acquire(connection, "SELECT 2");
    REQUIRE(a.has_value());
  }
  REQUIRE(cache.size() == 2);

  cache.clear();
  REQUIRE(cache.size() == 0);
  REQUIRE(cache.hits() == 0);
  REQUIRE(cache.misses() == 0);
  REQUIRE(cache.evictions() == 0);

  // Holding the leased entry is still safe; releasing it after clear must not crash
  // and must not put it back into the cache.
  held->release();
  REQUIRE(cache.size() == 0);

  // Subsequent acquires start fresh.
  auto fresh = cache.acquire(connection, "SELECT 1");
  REQUIRE(fresh.has_value());
  REQUIRE(cache.misses() == 1);
  REQUIRE(cache.size() == 1);
}

TEST_CASE("releasing after the cache is destroyed is safe", "[unit][storage][statement_cache]") {
  auto connection = open_memory();
  seed_schema(connection);

  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto lease = cache.acquire(connection, "SELECT 1");
  REQUIRE(lease.has_value());

  cache = storage::StatementCache{};  // drop the cache while the lease is out
  REQUIRE_FALSE(cache.valid());

  // The lease destructor must not touch the now-gone cache.
  lease->release();
  REQUIRE_FALSE(lease->valid());
}

TEST_CASE("lease release is idempotent", "[unit][storage][statement_cache]") {
  auto cache = storage::StatementCache::open(storage::StatementCacheOptions{.capacity = 2}).value();
  auto connection = open_memory();
  seed_schema(connection);

  auto lease = cache.acquire(connection, "SELECT 1");
  REQUIRE(lease.has_value());

  lease->release();
  REQUIRE_FALSE(lease->valid());
  lease->release();
  REQUIRE_FALSE(lease->valid());

  REQUIRE(cache.size() == 1);
}
