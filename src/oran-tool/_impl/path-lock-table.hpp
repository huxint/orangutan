#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
#include <oran/core/result.hpp>

namespace orangutan::tool::detail {

enum class PathLockMode : std::uint8_t {
  shared,
  exclusive,
};

class PathLockTable;

/// Move-only RAII handle that releases its lock on destruction. The guard does
/// not extend the lifetime of the owning `PathLocks`; dispatch callers retain
/// that resource until their guards have been released.
class PathLockGuard {
public:
  PathLockGuard() noexcept = default;
  PathLockGuard(const PathLockGuard&) = delete;
  PathLockGuard& operator=(const PathLockGuard&) = delete;
  PathLockGuard(PathLockGuard&& other) noexcept;
  PathLockGuard& operator=(PathLockGuard&& other) noexcept;
  ~PathLockGuard();

private:
  friend class PathLockTable;
  PathLockGuard(PathLockTable& table, std::string path, PathLockMode mode) noexcept;
  void reset() noexcept;

  PathLockTable* table_{nullptr};
  std::string path_{};
  PathLockMode mode_{PathLockMode::shared};
};

/// Strand-confined FIFO read/write locks. Only live holders and waiters retain
/// an entry; consecutive shared waiters may acquire together.
class PathLockTable {
public:
  PathLockTable() = default;
  PathLockTable(const PathLockTable&) = delete;
  PathLockTable& operator=(const PathLockTable&) = delete;

  /// Acquire a lock keyed by `path` in the requested mode. Returns a guard on
  /// success, or `Error::cancelled` if the awaiting coroutine was cancelled
  /// while waiting. The path is moved in to avoid a copy on the hot path.
  [[nodiscard]] async::Awaitable<core::Result<PathLockGuard>>
  acquire(asio::any_io_executor exec, std::string path, PathLockMode mode);

  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

private:
  friend class PathLockGuard;

  struct Waiter {
    PathLockMode mode;
    std::shared_ptr<async::Channel<std::monostate>> wake;
  };

  struct Entry {
    std::size_t readers{0};
    bool writer{false};
    std::deque<Waiter> waiters{};
  };

  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }

    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
      return (*this)(std::string_view{value});
    }
  };

  using Entries = std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>>;

  void release(std::string_view path, PathLockMode mode);
  void advance(Entries::iterator entry);

  Entries entries_;
};

}  // namespace orangutan::tool::detail
