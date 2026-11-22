// include/oran/io/fingerprint.hpp — cheap, byte-stable file identity.
//
// `FileFingerprint` is the lowest-cost identity primitive in `oran-io`:
// size + ns-resolution mtime, plus a hook for an optional SHA-256 content
// hash that a future slice will populate when callers opt in. Spec 0011
// (file-view system) names this shape; the agent-loop ranges, change
// detection, and `if_version` paths all consume it.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

struct stat;

namespace orangutan::io {

class ReadOnlyFile;

/// Stable, byte-cheap file identity. `size_bytes` and `mtime_ns` are
/// populated unconditionally; `sha256` is reserved for a future slice
/// (`compute_file_fingerprint(path, ComputeFingerprintOptions{.compute_hash=true})`)
/// and stays `nullopt` for now.
struct FileFingerprint {
  std::uintmax_t size_bytes{0};
  std::uint64_t mtime_ns{0};
  std::optional<std::string> sha256{};

  friend bool operator==(const FileFingerprint&, const FileFingerprint&) = default;
};

/// Build a `FileFingerprint` from an already-taken `fstat(2)` result without
/// re-resolving a pathname. The anchored-mutation commit path uses this to run
/// a freshness verifier in the same critical section as the rename.
[[nodiscard]] FileFingerprint fingerprint_from_stat(const struct stat& status) noexcept;

/// Read a file's metadata fingerprint. Returns:
///
///   * `Error::invalid_argument` — `path` is empty.
///   * `Error::not_found` — `path` does not exist.
///   * `Error::io` — `path` is not a regular file, or `stat` failed.
[[nodiscard]] core::Result<FileFingerprint> compute_file_fingerprint(std::string_view path);

/// Read metadata from an already-authorized file descriptor without
/// re-resolving its pathname.
[[nodiscard]] core::Result<FileFingerprint> compute_file_fingerprint(const ReadOnlyFile& file);

}  // namespace orangutan::io
