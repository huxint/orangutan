#pragma once

#include <memory>

namespace orangutan::tool {

namespace detail {
class PathLockTable;
}

/// Shared path exclusion for dispatches on one coordinating strand. The owner
/// retains this resource until every borrowing dispatch finishes cleanup.
class PathLocks {
public:
  PathLocks();
  ~PathLocks();

  PathLocks(const PathLocks&) = delete;
  PathLocks& operator=(const PathLocks&) = delete;

private:
  friend class Registry;
  std::unique_ptr<detail::PathLockTable> table_;
};

}  // namespace orangutan::tool
