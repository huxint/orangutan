// src/oran-storage/statement_cache.cpp — per-connection prepared-statement cache.

#include <oran/storage/statement_cache.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <oran/core/error.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

namespace {

struct CacheEntry {
  std::string sql;
  Statement stmt;
  bool leased{false};
  bool orphaned{false};
};

using EntryPtr = std::shared_ptr<CacheEntry>;
using EntryList = std::list<EntryPtr>;

[[nodiscard]] core::Error cache_not_open_error() {
  return core::Error{core::ErrorKind::conflict, "statement cache is not open"};
}

[[nodiscard]] core::Error already_leased_error(std::string_view sql) {
  return core::Error{core::ErrorKind::conflict, "statement is already leased"}.with("sql", std::string{sql});
}

[[nodiscard]] core::Error empty_sql_error() {
  return core::Error::invalid_argument("sql must not be empty");
}

}  // namespace

struct CachedStatement::Lease {
  std::weak_ptr<StatementCache::State> cache;
  EntryPtr entry;
  bool released{false};
};

struct StatementCache::State : std::enable_shared_from_this<State> {
  std::size_t capacity{0};
  EntryList lru;
  std::unordered_map<std::string, EntryList::iterator> by_sql;
  std::size_t hits{0};
  std::size_t misses{0};
  std::size_t evictions{0};

  [[nodiscard]] CachedStatement build_lease(EntryPtr entry) {
    auto lease = std::make_shared<CachedStatement::Lease>();
    lease->cache = weak_from_this();
    lease->entry = std::move(entry);
    return CachedStatement{std::move(lease)};
  }
};

StatementCache::StatementCache() noexcept = default;

StatementCache::~StatementCache() = default;

StatementCache::StatementCache(StatementCache&&) noexcept = default;

StatementCache& StatementCache::operator=(StatementCache&&) noexcept = default;

core::Result<StatementCache> StatementCache::open(StatementCacheOptions options) {
  if (options.capacity == 0) {
    return std::unexpected(core::Error::invalid_argument("statement cache capacity must be greater than zero"));
  }
  auto state = std::make_shared<State>();
  state->capacity = options.capacity;

  StatementCache cache;
  cache.state_ = std::move(state);
  return cache;
}

bool StatementCache::valid() const noexcept {
  return state_ != nullptr;
}

std::size_t StatementCache::capacity() const noexcept {
  return state_ ? state_->capacity : 0;
}

std::size_t StatementCache::size() const noexcept {
  return state_ ? state_->lru.size() : 0;
}

std::size_t StatementCache::hits() const noexcept {
  return state_ ? state_->hits : 0;
}

std::size_t StatementCache::misses() const noexcept {
  return state_ ? state_->misses : 0;
}

std::size_t StatementCache::evictions() const noexcept {
  return state_ ? state_->evictions : 0;
}

core::Result<CachedStatement> StatementCache::acquire(Connection& connection, std::string_view sql) {
  if (!state_) {
    return std::unexpected(cache_not_open_error());
  }
  if (sql.empty()) {
    return std::unexpected(empty_sql_error());
  }

  auto& state = *state_;
  std::string key{sql};

  if (auto it = state.by_sql.find(key); it != state.by_sql.end()) {
    auto entry = *it->second;
    if (entry->leased) {
      return std::unexpected(already_leased_error(sql));
    }

    state.lru.splice(state.lru.begin(), state.lru, it->second);
    it->second = state.lru.begin();

    if (auto reset = entry->stmt.reset(); !reset) {
      state.lru.erase(it->second);
      state.by_sql.erase(it);
      return std::unexpected(std::move(reset).error().with("sql", std::move(key)));
    }
    if (auto cleared = entry->stmt.clear_bindings(); !cleared) {
      state.lru.erase(it->second);
      state.by_sql.erase(it);
      return std::unexpected(std::move(cleared).error().with("sql", std::move(key)));
    }

    entry->leased = true;
    state.hits += 1;
    return state.build_lease(entry);
  }

  auto prepared = connection.prepare(sql);
  if (!prepared) {
    state.misses += 1;
    return std::unexpected(std::move(prepared).error());
  }
  state.misses += 1;

  auto entry = std::make_shared<CacheEntry>();
  entry->sql = key;
  entry->stmt = std::move(*prepared);
  entry->leased = true;

  if (state.lru.size() < state.capacity) {
    state.lru.push_front(entry);
    state.by_sql.emplace(entry->sql, state.lru.begin());
    return state.build_lease(std::move(entry));
  }

  auto victim = std::ranges::find_last_if(state.lru, [](const EntryPtr& e) noexcept { return !e->leased; });
  if (victim.empty()) {
    entry->orphaned = true;
    return state.build_lease(std::move(entry));
  }
  auto victim_it = victim.begin();

  state.by_sql.erase((*victim_it)->sql);
  state.lru.erase(victim_it);
  state.evictions += 1;

  state.lru.push_front(entry);
  state.by_sql.emplace(entry->sql, state.lru.begin());
  return state.build_lease(std::move(entry));
}

void StatementCache::clear() noexcept {
  if (!state_) {
    return;
  }
  auto& state = *state_;
  state.by_sql.clear();
  for (auto it = state.lru.begin(); it != state.lru.end();) {
    if ((*it)->leased) {
      (*it)->orphaned = true;
    }
    it = state.lru.erase(it);
  }
  state.hits = 0;
  state.misses = 0;
  state.evictions = 0;
}

CachedStatement::CachedStatement(std::shared_ptr<Lease> lease) noexcept : lease_{std::move(lease)} {}

CachedStatement::~CachedStatement() {
  release();
}

CachedStatement::CachedStatement(CachedStatement&& other) noexcept : lease_{std::move(other.lease_)} {}

CachedStatement& CachedStatement::operator=(CachedStatement&& other) noexcept {
  if (this != &other) {
    release();
    lease_ = std::move(other.lease_);
  }
  return *this;
}

bool CachedStatement::valid() const noexcept {
  return lease_ && !lease_->released && lease_->entry != nullptr;
}

Statement& CachedStatement::statement() noexcept {
  return lease_->entry->stmt;
}

const Statement& CachedStatement::statement() const noexcept {
  return lease_->entry->stmt;
}

void CachedStatement::release() noexcept {
  if (!lease_ || lease_->released) {
    lease_.reset();
    return;
  }
  lease_->released = true;
  auto entry = std::move(lease_->entry);
  auto cache = lease_->cache.lock();
  lease_.reset();

  if (!entry || entry->orphaned || !cache) {
    return;
  }

  auto reset = entry->stmt.reset();
  auto cleared = entry->stmt.clear_bindings();
  if (!reset || !cleared) {
    if (auto it = cache->by_sql.find(entry->sql); it != cache->by_sql.end() && *it->second == entry) {
      cache->lru.erase(it->second);
      cache->by_sql.erase(it);
    }
    return;
  }

  entry->leased = false;
}

}  // namespace orangutan::storage
