#include <oran/tool/path-locks.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/any_io_executor.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

#include "_impl/path-lock-table.hpp"

namespace orangutan::tool {

PathLocks::PathLocks() : table_{std::make_unique<detail::PathLockTable>()} {}

PathLocks::~PathLocks() = default;

namespace detail {

PathLockGuard::PathLockGuard(PathLockTable& table, std::string path, PathLockMode mode) noexcept
    : table_{&table}, path_{std::move(path)}, mode_{mode} {}

PathLockGuard::PathLockGuard(PathLockGuard&& other) noexcept
    : table_{std::exchange(other.table_, nullptr)}, path_{std::move(other.path_)}, mode_{other.mode_} {}

PathLockGuard& PathLockGuard::operator=(PathLockGuard&& other) noexcept {
  if (this != &other) {
    reset();
    table_ = std::exchange(other.table_, nullptr);
    path_ = std::move(other.path_);
    mode_ = other.mode_;
  }
  return *this;
}

PathLockGuard::~PathLockGuard() {
  reset();
}

void PathLockGuard::reset() noexcept {
  if (table_ != nullptr) {
    table_->release(path_, mode_);
    table_ = nullptr;
  }
}

async::Awaitable<core::Result<PathLockGuard>>
PathLockTable::acquire(asio::any_io_executor exec, std::string path, PathLockMode mode) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }
  std::shared_ptr<async::Channel<std::monostate>> wake;

  {
    auto& entry = entries_.try_emplace(path).first->second;
    const bool can_take_now =
        entry.waiters.empty() && !entry.writer && (mode == PathLockMode::shared || entry.readers == 0);

    if (can_take_now) {
      if (mode == PathLockMode::shared) {
        ++entry.readers;
      } else {
        entry.writer = true;
      }
      co_return PathLockGuard{*this, std::move(path), mode};
    }

    wake = std::make_shared<async::Channel<std::monostate>>(exec, 1);
    entry.waiters.push_back(Waiter{.mode = mode, .wake = wake});
  }

  auto receive_result = co_await wake->receive();
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    receive_result = std::unexpected(core::Error::cancelled());
  }

  // Inserts during suspension can invalidate iterators. A queued waiter or
  // its reserved reader/writer permit keeps this entry alive until we resume.
  auto it = entries_.find(path);
  if (it == entries_.end()) {
    co_return std::unexpected(core::Error::internal("path_lock_table: entry vanished while waiting"));
  }

  if (!receive_result) {
    const auto erased =
        std::erase_if(it->second.waiters, [&wake](const Waiter& waiter) { return waiter.wake == wake; });
    if (erased == 0) {
      // A permit was reserved while the cancelled receive's completion was
      // queued. Return that reservation before handing the path onward.
      release(path, mode);
    } else {
      advance(it);
    }
    co_return std::unexpected(std::move(receive_result).error());
  }

  co_return PathLockGuard{*this, std::move(path), mode};
}

void PathLockTable::release(std::string_view path, PathLockMode mode) {
  auto it = entries_.find(path);
  if (it == entries_.end()) {
    return;
  }
  Entry& entry = it->second;

  if (mode == PathLockMode::shared) {
    if (entry.readers > 0) {
      --entry.readers;
    }
  } else {
    entry.writer = false;
  }
  advance(it);
}

void PathLockTable::advance(Entries::iterator it) {
  auto& entry = it->second;
  while (!entry.waiters.empty()) {
    const auto& front = entry.waiters.front();
    const bool can_wake =
        (front.mode == PathLockMode::exclusive) ? (entry.readers == 0 && !entry.writer) : !entry.writer;
    if (!can_wake) {
      break;
    }

    const auto mode = front.mode;
    auto channel = front.wake;
    entry.waiters.pop_front();

    // Reserve before posting the completion so a new acquire cannot bypass
    // this waiter and the entry cannot be reclaimed while it resumes.
    if (mode == PathLockMode::shared) {
      ++entry.readers;
    } else {
      entry.writer = true;
    }

    // The channel is sized 1 and never sent to before this point, so
    // `try_send` cannot back-pressure. A cancelled waiter's channel still
    // accepts the permit; the cancellation arm in `acquire` reconciles.
    [[maybe_unused]] auto sent = channel->try_send(std::monostate{});

    if (mode == PathLockMode::exclusive) {
      break;
    }
  }

  if (entry.readers == 0 && !entry.writer && entry.waiters.empty()) {
    entries_.erase(it);
  }
}

}  // namespace detail
}  // namespace orangutan::tool
