// src/oran-storage/pool.cpp — async writer/reader connection pool implementation.

#include <oran/storage/pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/append.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/dispatch.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

[[nodiscard]] core::Error pool_closed_error() {
  return core::Error{core::ErrorKind::conflict, "pool is not open"};
}

}  // namespace

struct Pool::State : std::enable_shared_from_this<Pool::State> {
  explicit State(asio::any_io_executor exec) : executor{std::move(exec)} {}

  using WriterCompletion = std::move_only_function<void(core::Result<WriterLease>)>;
  using ReaderCompletion = std::move_only_function<void(core::Result<ReaderLease>)>;
  using DeferredAction = std::move_only_function<void()>;

  enum class WaiterStatus : std::uint8_t {
    pending,
    cancelled,
    granted,
  };

  struct WriterWaiter {
    WriterCompletion complete;
    WaiterStatus status{WaiterStatus::pending};
  };
  struct ReaderWaiter {
    ReaderCompletion complete;
    WaiterStatus status{WaiterStatus::pending};
  };

  asio::any_io_executor executor;
  mutable std::mutex mutex;

  Connection writer;
  std::vector<Connection> readers;
  StatementCache writer_cache;
  std::vector<StatementCache> reader_caches;

  bool writer_busy{false};
  std::deque<std::size_t> free_readers;
  std::deque<std::shared_ptr<WriterWaiter>> writer_waiters;
  std::deque<std::shared_ptr<ReaderWaiter>> reader_waiters;
  [[nodiscard]] WriterLease make_writer_lease();
  [[nodiscard]] ReaderLease make_reader_lease(std::size_t slot);

  void release_writer() {
    std::shared_ptr<WriterWaiter> waiter;
    {
      const std::scoped_lock lock{mutex};
      if (!writer_waiters.empty()) {
        waiter = std::move(writer_waiters.front());
        writer_waiters.pop_front();
      } else {
        writer_busy = false;
      }
    }
    if (waiter) {
      const auto self = shared_from_this();
      asio::post(executor, [self, waiter = std::move(waiter)]() mutable { self->grant_writer_waiter(waiter); });
    }
  }

  void grant_writer_waiter(const std::shared_ptr<WriterWaiter>& waiter) {
    WriterCompletion completion;
    bool return_reserved_writer = false;
    {
      const std::scoped_lock lock{mutex};
      if (waiter->status == WaiterStatus::pending) {
        waiter->status = WaiterStatus::granted;
        completion = std::move(waiter->complete);
      } else {
        return_reserved_writer = true;
      }
    }
    if (completion) {
      completion(core::Result<WriterLease>{make_writer_lease()});
    } else if (return_reserved_writer) {
      release_writer();
    }
  }

  void release_reader(std::size_t slot) {
    std::shared_ptr<ReaderWaiter> waiter;
    {
      const std::scoped_lock lock{mutex};
      if (!reader_waiters.empty()) {
        waiter = std::move(reader_waiters.front());
        reader_waiters.pop_front();
      } else {
        free_readers.push_back(slot);
        return;
      }
    }
    if (waiter) {
      const auto self = shared_from_this();
      asio::post(executor,
                 [self, waiter = std::move(waiter), slot]() mutable { self->grant_reader_waiter(waiter, slot); });
    }
  }

  void grant_reader_waiter(const std::shared_ptr<ReaderWaiter>& waiter, std::size_t slot) {
    ReaderCompletion completion;
    bool return_reserved_reader = false;
    {
      const std::scoped_lock lock{mutex};
      if (waiter->status == WaiterStatus::pending) {
        waiter->status = WaiterStatus::granted;
        completion = std::move(waiter->complete);
      } else {
        return_reserved_reader = true;
      }
    }
    if (completion) {
      completion(core::Result<ReaderLease>{make_reader_lease(slot)});
    } else if (return_reserved_reader) {
      release_reader(slot);
    }
  }

  void cancel_writer_waiter(const std::shared_ptr<WriterWaiter>& waiter) {
    WriterCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      if (waiter->status != WaiterStatus::pending) {
        return;
      }
      waiter->status = WaiterStatus::cancelled;
      completion = std::move(waiter->complete);
      if (const auto queued = std::ranges::find(writer_waiters, waiter); queued != writer_waiters.end()) {
        writer_waiters.erase(queued);
      }
    }
    if (completion) {
      asio::post(executor, [completion = std::move(completion)]() mutable {
        completion(std::unexpected(core::Error::cancelled()));
      });
    }
  }

  void cancel_reader_waiter(const std::shared_ptr<ReaderWaiter>& waiter) {
    ReaderCompletion completion;
    {
      const std::scoped_lock lock{mutex};
      if (waiter->status != WaiterStatus::pending) {
        return;
      }
      waiter->status = WaiterStatus::cancelled;
      completion = std::move(waiter->complete);
      if (const auto queued = std::ranges::find(reader_waiters, waiter); queued != reader_waiters.end()) {
        reader_waiters.erase(queued);
      }
    }
    if (completion) {
      asio::post(executor, [completion = std::move(completion)]() mutable {
        completion(std::unexpected(core::Error::cancelled()));
      });
    }
  }

  template <typename Handler>
  void async_acquire_writer(asio::cancellation_state cancellation, Handler&& handler) {
    auto cancel_slot = asio::get_associated_cancellation_slot(handler);
    auto caller = asio::get_associated_executor(handler, executor);
    auto complete = [caller, handler = std::forward<Handler>(handler)](core::Result<WriterLease> result) mutable {
      asio::dispatch(caller, asio::append(std::move(handler), std::move(result)));
    };

    DeferredAction action;
    {
      const std::scoped_lock lock{mutex};
      if (!writer_busy) {
        writer_busy = true;
        auto lease = make_writer_lease();
        action = [complete = std::move(complete), lease = std::move(lease)]() mutable {
          complete(core::Result<WriterLease>{std::move(lease)});
        };
      } else {
        auto waiter = std::make_shared<WriterWaiter>(WriterWaiter{
            .complete = WriterCompletion{std::move(complete)},
        });
        writer_waiters.push_back(waiter);
        // Install the cancel handler after the waiter is in the queue, under
        // the same mutex. Installing earlier would leave a window where a
        // cancellation could fire, scan an empty queue, and be silently
        // dropped while the waiter is later pushed and never cancelled.
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = weak_from_this();
          const std::weak_ptr<WriterWaiter> weak_waiter = waiter;
          cancel_slot.assign([weak, weak_waiter](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock(); state) {
              if (auto locked_waiter = weak_waiter.lock()) {
                state->cancel_writer_waiter(locked_waiter);
              }
            }
          });
        }
        if (cancellation.cancelled() != asio::cancellation_type::none && waiter->status == WaiterStatus::pending) {
          waiter->status = WaiterStatus::cancelled;
          writer_waiters.pop_back();
          auto completion = std::move(waiter->complete);
          action = [completion = std::move(completion)]() mutable {
            completion(std::unexpected(core::Error::cancelled()));
          };
        }
      }
    }
    if (action) {
      asio::post(executor, std::move(action));
    }
  }

  template <typename Handler>
  void async_acquire_reader(asio::cancellation_state cancellation, Handler&& handler) {
    auto cancel_slot = asio::get_associated_cancellation_slot(handler);
    auto caller = asio::get_associated_executor(handler, executor);
    auto complete = [caller, handler = std::forward<Handler>(handler)](core::Result<ReaderLease> result) mutable {
      asio::dispatch(caller, asio::append(std::move(handler), std::move(result)));
    };

    DeferredAction action;
    {
      const std::scoped_lock lock{mutex};
      if (!free_readers.empty()) {
        const auto slot = free_readers.front();
        free_readers.pop_front();
        auto lease = make_reader_lease(slot);
        action = [complete = std::move(complete), lease = std::move(lease)]() mutable {
          complete(core::Result<ReaderLease>{std::move(lease)});
        };
      } else {
        auto waiter = std::make_shared<ReaderWaiter>(ReaderWaiter{
            .complete = ReaderCompletion{std::move(complete)},
        });
        reader_waiters.push_back(waiter);
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = weak_from_this();
          const std::weak_ptr<ReaderWaiter> weak_waiter = waiter;
          cancel_slot.assign([weak, weak_waiter](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock(); state) {
              if (auto locked_waiter = weak_waiter.lock()) {
                state->cancel_reader_waiter(locked_waiter);
              }
            }
          });
        }
        if (cancellation.cancelled() != asio::cancellation_type::none && waiter->status == WaiterStatus::pending) {
          waiter->status = WaiterStatus::cancelled;
          reader_waiters.pop_back();
          auto completion = std::move(waiter->complete);
          action = [completion = std::move(completion)]() mutable {
            completion(std::unexpected(core::Error::cancelled()));
          };
        }
      }
    }
    if (action) {
      asio::post(executor, std::move(action));
    }
  }
};

struct WriterLease::State {
  std::shared_ptr<Pool::State> pool;
  bool released{false};
};

struct ReaderLease::State {
  std::shared_ptr<Pool::State> pool;
  std::size_t slot{};
  bool released{false};
};

WriterLease Pool::State::make_writer_lease() {
  auto lease_state = std::make_shared<WriterLease::State>();
  lease_state->pool = shared_from_this();
  return WriterLease{std::move(lease_state)};
}

ReaderLease Pool::State::make_reader_lease(std::size_t slot) {
  auto lease_state = std::make_shared<ReaderLease::State>();
  lease_state->pool = shared_from_this();
  lease_state->slot = slot;
  return ReaderLease{std::move(lease_state), slot};
}

WriterLease::WriterLease(std::shared_ptr<State> state) noexcept : state_{std::move(state)} {}

WriterLease::~WriterLease() {
  release();
}

WriterLease::WriterLease(WriterLease&& other) noexcept : state_{std::move(other.state_)} {}

WriterLease& WriterLease::operator=(WriterLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
  }
  return *this;
}

bool WriterLease::valid() const noexcept {
  return state_ && !state_->released && state_->pool != nullptr;
}

Connection& WriterLease::connection() noexcept {
  return state_->pool->writer;
}

const Connection& WriterLease::connection() const noexcept {
  return state_->pool->writer;
}

StatementCache& WriterLease::statement_cache() noexcept {
  return state_->pool->writer_cache;
}

const StatementCache& WriterLease::statement_cache() const noexcept {
  return state_->pool->writer_cache;
}

void WriterLease::release() noexcept {
  if (!state_ || state_->released || state_->pool == nullptr) {
    state_.reset();
    return;
  }
  auto pool = std::move(state_->pool);
  state_->released = true;
  state_.reset();
  pool->release_writer();
}

ReaderLease::ReaderLease(std::shared_ptr<State> state, std::size_t slot) noexcept
    : state_{std::move(state)}, slot_{slot} {}

ReaderLease::~ReaderLease() {
  release();
}

ReaderLease::ReaderLease(ReaderLease&& other) noexcept : state_{std::move(other.state_)}, slot_{other.slot_} {
  other.slot_ = 0;
}

ReaderLease& ReaderLease::operator=(ReaderLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    slot_ = other.slot_;
    other.slot_ = 0;
  }
  return *this;
}

bool ReaderLease::valid() const noexcept {
  return state_ && !state_->released && state_->pool != nullptr;
}

std::size_t ReaderLease::slot() const noexcept {
  return slot_;
}

Connection& ReaderLease::connection() noexcept {
  return state_->pool->readers[state_->slot];
}

const Connection& ReaderLease::connection() const noexcept {
  return state_->pool->readers[state_->slot];
}

StatementCache& ReaderLease::statement_cache() noexcept {
  return state_->pool->reader_caches[state_->slot];
}

const StatementCache& ReaderLease::statement_cache() const noexcept {
  return state_->pool->reader_caches[state_->slot];
}

void ReaderLease::release() noexcept {
  if (!state_ || state_->released || state_->pool == nullptr) {
    state_.reset();
    return;
  }
  auto pool = std::move(state_->pool);
  auto slot = state_->slot;
  state_->released = true;
  state_.reset();
  pool->release_reader(slot);
}

Pool::Pool() noexcept = default;

Pool::~Pool() = default;

Pool::Pool(Pool&&) noexcept = default;

Pool& Pool::operator=(Pool&&) noexcept = default;

core::Result<Pool> Pool::open(asio::any_io_executor executor, PoolOptions options) {
  if (options.path.empty()) {
    return std::unexpected(core::Error::invalid_argument("pool path must not be empty"));
  }
  if (options.reader_count == 0) {
    return std::unexpected(core::Error::invalid_argument("pool reader_count must be greater than zero"));
  }
  if (options.statement_cache_capacity == 0) {
    return std::unexpected(core::Error::invalid_argument("pool statement_cache_capacity must be greater than zero"));
  }

  auto state = std::make_shared<State>(std::move(executor));

  auto writer = Connection::open(
      ConnectionOptions{
          .path = options.path,
          .mode = OpenMode::read_write_create,
          .busy_timeout_ms = options.busy_timeout_ms,
          .enable_wal = options.enable_wal,
          .enforce_foreign_keys = options.enforce_foreign_keys,
      });
  if (!writer) {
    return std::unexpected(std::move(writer.error()).with("pool_role", "writer"));
  }
  state->writer = std::move(*writer);

  auto writer_cache = StatementCache::open(StatementCacheOptions{.capacity = options.statement_cache_capacity});
  if (!writer_cache) {
    return std::unexpected(std::move(writer_cache.error()).with("pool_role", "writer_cache"));
  }
  state->writer_cache = std::move(*writer_cache);

  state->readers.reserve(options.reader_count);
  state->reader_caches.reserve(options.reader_count);
  for (std::size_t i = 0; i < options.reader_count; ++i) {
    auto reader = Connection::open(
        ConnectionOptions{
            .path = options.path,
            .mode = OpenMode::read_only,
            .busy_timeout_ms = options.busy_timeout_ms,
            .enable_wal = false,
            .enforce_foreign_keys = options.enforce_foreign_keys,
        });
    if (!reader) {
      return std::unexpected(
          std::move(reader.error()).with("pool_role", "reader").with("pool_slot", std::to_string(i)));
    }
    state->readers.push_back(std::move(*reader));

    auto reader_cache = StatementCache::open(StatementCacheOptions{.capacity = options.statement_cache_capacity});
    if (!reader_cache) {
      return std::unexpected(
          std::move(reader_cache.error()).with("pool_role", "reader_cache").with("pool_slot", std::to_string(i)));
    }
    state->reader_caches.push_back(std::move(*reader_cache));
    state->free_readers.push_back(i);
  }

  Pool pool;
  pool.state_ = std::move(state);
  return pool;
}

bool Pool::valid() const noexcept {
  return state_ != nullptr;
}

std::size_t Pool::reader_count() const noexcept {
  return state_ ? state_->readers.size() : 0;
}

std::size_t Pool::readers_available() const noexcept {
  if (!state_) {
    return 0;
  }
  const std::scoped_lock lock{state_->mutex};
  return state_->free_readers.size();
}

bool Pool::writer_busy() const noexcept {
  if (!state_) {
    return false;
  }
  const std::scoped_lock lock{state_->mutex};
  return state_->writer_busy;
}

async::Awaitable<core::Result<WriterLease>> Pool::acquire_writer() {
  if (!state_) {
    co_return std::unexpected(pool_closed_error());
  }
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto state = state_;
  co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(core::Result<WriterLease>)>(
      [state, cancellation]<typename Handler>(Handler&& handler) mutable {
        state->async_acquire_writer(cancellation, std::forward<Handler>(handler));
      },
      asio::use_awaitable);
}

async::Awaitable<core::Result<ReaderLease>> Pool::acquire_reader() {
  if (!state_) {
    co_return std::unexpected(pool_closed_error());
  }
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto state = state_;
  co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(core::Result<ReaderLease>)>(
      [state, cancellation]<typename Handler>(Handler&& handler) mutable {
        state->async_acquire_reader(cancellation, std::forward<Handler>(handler));
      },
      asio::use_awaitable);
}

}  // namespace orangutan::storage
