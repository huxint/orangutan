#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/io/range.hpp>

namespace orangutan::io {

class ReadOnlyFile;
class FileMutation;

enum class WriteMode : std::uint8_t {
  truncate,
  append,
  fail_if_exists,
};

enum class WriteTextDurability : std::uint8_t {
  rename_only,
  fsync_file,
  fsync_file_and_parent,
};

struct ReadTextOptions {
  std::uintmax_t max_bytes{16U * 1024U * 1024U};
  /// Optional line- or byte-range request. When unset the helper returns
  /// the entire file (subject to `max_bytes`). See `FileRange` for the
  /// mutual-exclusion contract; invalid ranges reject with
  /// `Error::invalid_argument`.
  std::optional<FileRange> range{};
};

struct WriteTextOptions {
  WriteMode mode{WriteMode::truncate};
  bool create_parent_directories{false};
  /// Require truncate mode and permit durability or commit verification.
  /// Truncate always stages a sibling file before the atomic rename.
  bool atomic{false};
  /// `fsync_file` syncs the staged file; `fsync_file_and_parent` also syncs
  /// the parent after rename. Non-default durability requires `atomic`.
  WriteTextDurability durability{WriteTextDurability::rename_only};
  /// Verify the current target fingerprint immediately before rename;
  /// failure leaves the target untouched. Runs on the worker and must not
  /// block. Requires an existing target, truncate mode and `atomic`.
  std::move_only_function<core::Result<void>(const FileFingerprint&)> verify_before_commit{};
};

/// Read a trusted host path; truncation returns `invalid_argument`.
[[nodiscard]] async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions options = {});

/// Open a trusted host path on the worker and read its bounded contents.
/// Size or mtime drift retries once for whole files smaller than 64 KiB;
/// larger or ranged reads return `conflict`. Each call owns a descriptor.
[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, std::string path, ReadTextOptions options = {});

/// Range-aware read from an already-authorized regular file. This overload
/// never reopens the diagnostic pathname; the held descriptor is the file
/// identity throughout the read.
[[nodiscard]] async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, ReadOnlyFile file, ReadTextOptions options = {});

/// Write through a pinned directory authority. Truncate stages a sibling
/// temporary file and revalidates the target identity immediately before
/// rename; append and fail-if-exists remain anchored to the pinned parent.
[[nodiscard]] async::Awaitable<core::Result<void>> write_text_file(asio::any_io_executor executor,
                                                                   FileMutation mutation,
                                                                   std::string contents,
                                                                   WriteTextOptions options = {});

}  // namespace orangutan::io
