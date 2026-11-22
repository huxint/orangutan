// include/oran/storage/statement_cache.hpp — per-connection prepared-statement cache.

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include <oran/core/result.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

struct StatementCacheOptions {
  std::size_t capacity{32};
};

class CachedStatement;

class StatementCache {
public:
  StatementCache() noexcept;
  ~StatementCache();

  StatementCache(const StatementCache&) = delete;
  StatementCache& operator=(const StatementCache&) = delete;
  StatementCache(StatementCache&&) noexcept;
  StatementCache& operator=(StatementCache&&) noexcept;

  [[nodiscard]] static core::Result<StatementCache> open(StatementCacheOptions options);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t hits() const noexcept;
  [[nodiscard]] std::size_t misses() const noexcept;
  [[nodiscard]] std::size_t evictions() const noexcept;

  [[nodiscard]] core::Result<CachedStatement> acquire(Connection& connection, std::string_view sql);

  void clear() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;

  friend class CachedStatement;
};

class CachedStatement {
public:
  CachedStatement() noexcept = default;
  ~CachedStatement();

  CachedStatement(const CachedStatement&) = delete;
  CachedStatement& operator=(const CachedStatement&) = delete;
  CachedStatement(CachedStatement&& other) noexcept;
  CachedStatement& operator=(CachedStatement&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Statement& statement() noexcept;
  [[nodiscard]] const Statement& statement() const noexcept;

  void release() noexcept;

private:
  struct Lease;
  explicit CachedStatement(std::shared_ptr<Lease> lease) noexcept;

  std::shared_ptr<Lease> lease_;

  friend class StatementCache;
};

}  // namespace orangutan::storage
