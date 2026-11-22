// include/oran/storage/pool.hpp — async writer/reader connection pool.

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

class StatementCache;

struct PoolOptions {
  std::string path;
  std::size_t reader_count{2};
  std::size_t statement_cache_capacity{32};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

class Pool;

class WriterLease {
public:
  WriterLease() noexcept = default;
  ~WriterLease();

  WriterLease(const WriterLease&) = delete;
  WriterLease& operator=(const WriterLease&) = delete;
  WriterLease(WriterLease&& other) noexcept;
  WriterLease& operator=(WriterLease&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Connection& connection() noexcept;
  [[nodiscard]] const Connection& connection() const noexcept;
  [[nodiscard]] StatementCache& statement_cache() noexcept;
  [[nodiscard]] const StatementCache& statement_cache() const noexcept;

  void release() noexcept;

private:
  struct State;
  WriterLease(std::shared_ptr<State> state) noexcept;
  std::shared_ptr<State> state_;

  friend class Pool;
};

class ReaderLease {
public:
  ReaderLease() noexcept = default;
  ~ReaderLease();

  ReaderLease(const ReaderLease&) = delete;
  ReaderLease& operator=(const ReaderLease&) = delete;
  ReaderLease(ReaderLease&& other) noexcept;
  ReaderLease& operator=(ReaderLease&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t slot() const noexcept;
  [[nodiscard]] Connection& connection() noexcept;
  [[nodiscard]] const Connection& connection() const noexcept;
  [[nodiscard]] StatementCache& statement_cache() noexcept;
  [[nodiscard]] const StatementCache& statement_cache() const noexcept;

  void release() noexcept;

private:
  struct State;
  ReaderLease(std::shared_ptr<State> state, std::size_t slot) noexcept;
  std::shared_ptr<State> state_;
  std::size_t slot_{};

  friend class Pool;
};

class Pool {
public:
  Pool() noexcept;
  ~Pool();

  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) noexcept;
  Pool& operator=(Pool&&) noexcept;

  /// The executor drives lease availability; each completion resumes on the
  /// requesting coroutine's executor. SQL runs wherever that coroutine runs.
  [[nodiscard]] static core::Result<Pool> open(asio::any_io_executor executor, PoolOptions options);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t reader_count() const noexcept;
  [[nodiscard]] std::size_t readers_available() const noexcept;
  [[nodiscard]] bool writer_busy() const noexcept;

  [[nodiscard]] async::Awaitable<core::Result<WriterLease>> acquire_writer();
  [[nodiscard]] async::Awaitable<core::Result<ReaderLease>> acquire_reader();

private:
  struct State;
  std::shared_ptr<State> state_;

  friend class WriterLease;
  friend class ReaderLease;
};

}  // namespace orangutan::storage
