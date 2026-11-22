#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::io {

class FileLock {
public:
  ~FileLock();
  FileLock(FileLock&&) noexcept;
  FileLock& operator=(FileLock&&) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

private:
  friend class PrivateDirectory;
  explicit FileLock(int descriptor) noexcept;
  int descriptor_{-1};
};

/// Pinned, owner-private application directory. Synchronous operations belong
/// on the blocking executor or at process startup. Names are single components.
class PrivateDirectory {
public:
  [[nodiscard]] static core::Result<PrivateDirectory> open(std::string path);
  ~PrivateDirectory();
  PrivateDirectory(PrivateDirectory&&) noexcept;
  PrivateDirectory& operator=(PrivateDirectory&&) noexcept;
  PrivateDirectory(const PrivateDirectory&) = delete;
  PrivateDirectory& operator=(const PrivateDirectory&) = delete;

  [[nodiscard]] core::Result<std::optional<std::string>> read(std::string_view name, std::size_t max_bytes) const;
  /// Atomically replace a private file, syncing its contents and directory.
  [[nodiscard]] core::Result<void> write(std::string_view name, std::string_view contents) const;
  [[nodiscard]] core::Result<FileLock> lock(std::string_view name) const;

private:
  explicit PrivateDirectory(int descriptor) noexcept;
  int descriptor_{-1};
};

}  // namespace orangutan::io
