// src/oran-io/fingerprint.cpp — size + mtime fingerprint implementation.

#include <oran/io/fingerprint.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>

#include <oran/core/error.hpp>
#include <oran/io/directory_authority.hpp>

namespace orangutan::io {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Error
filesystem_error(std::string message, const std::filesystem::path& path, const std::error_code& ec) {
  return Error::io(std::move(message)).with("path", path.string()).with("detail", ec.message());
}

/// Convert `std::filesystem::file_time_type` to nanoseconds since the Unix
/// epoch. The conversion goes via `clock_cast` so the result is independent
/// of whether the filesystem clock equals the system clock on this
/// implementation (libstdc++ ties them on Linux; libc++ does not).
[[nodiscard]] std::uint64_t to_unix_nanoseconds(std::filesystem::file_time_type ftt) noexcept {
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftt);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
  return ns < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(ns);
}

[[nodiscard]] std::uint64_t stat_mtime_nanoseconds(const struct stat& status) noexcept {
#if defined(__APPLE__)
  const auto seconds = status.st_mtimespec.tv_sec;
  const auto nanoseconds = status.st_mtimespec.tv_nsec;
#else
  const auto seconds = status.st_mtim.tv_sec;
  const auto nanoseconds = status.st_mtim.tv_nsec;
#endif
  if (seconds < 0) {
    return 0;
  }
  constexpr auto kNanosecondsPerSecond = std::uint64_t{1'000'000'000};
  return static_cast<std::uint64_t>(seconds) * kNanosecondsPerSecond + static_cast<std::uint64_t>(nanoseconds);
}

}  // namespace

Result<FileFingerprint> compute_file_fingerprint(std::string_view path) {
  if (path.empty()) {
    return std::unexpected(Error::invalid_argument("fingerprint path must not be empty"));
  }

  const auto fs_path = std::filesystem::path{std::string{path}};
  std::error_code ec;
  const auto status = std::filesystem::status(fs_path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return std::unexpected(filesystem_error("failed to inspect file for fingerprint", fs_path, ec));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("file does not exist").with("path", std::string{path}));
  }
  if (!std::filesystem::is_regular_file(status)) {
    return std::unexpected(Error::io("fingerprint target is not a regular file").with("path", std::string{path}));
  }

  const auto size = std::filesystem::file_size(fs_path, ec);
  if (ec) {
    return std::unexpected(filesystem_error("failed to read file size for fingerprint", fs_path, ec));
  }

  const auto mtime = std::filesystem::last_write_time(fs_path, ec);
  if (ec) {
    return std::unexpected(filesystem_error("failed to read mtime for fingerprint", fs_path, ec));
  }

  return FileFingerprint{
      .size_bytes = size,
      .mtime_ns = to_unix_nanoseconds(mtime),
      .sha256 = std::nullopt,
  };
}

Result<FileFingerprint> compute_file_fingerprint(const ReadOnlyFile& file) {
  if (file.native_handle() < 0) {
    return std::unexpected(Error::invalid_argument("fingerprint file handle must be valid"));
  }

  struct stat status{};
  errno = 0;
  if (::fstat(file.native_handle(), &status) != 0) {
    return std::unexpected(Error::io("failed to inspect file descriptor for fingerprint")
                               .with("path", std::string{file.display_path()})
                               .with("errno", std::to_string(errno))
                               .with("detail", std::generic_category().message(errno)));
  }
  if (!S_ISREG(status.st_mode)) {
    return std::unexpected(
        Error::io("fingerprint target is not a regular file").with("path", std::string{file.display_path()}));
  }

  return fingerprint_from_stat(status);
}

FileFingerprint fingerprint_from_stat(const struct stat& status) noexcept {
  return FileFingerprint{
      .size_bytes = static_cast<std::uintmax_t>(status.st_size),
      .mtime_ns = stat_mtime_nanoseconds(status),
      .sha256 = std::nullopt,
  };
}

}  // namespace orangutan::io
