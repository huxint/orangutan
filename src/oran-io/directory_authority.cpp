// src/oran-io/directory_authority.cpp — Linux-first dirfd path resolution.

#include <oran/io/directory_authority.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

#include <oran/core/error.hpp>
#include <oran/io/file.hpp>

namespace orangutan::io {
namespace {

constexpr std::size_t kResolutionRaceRetries = 3U;

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_{other.release()} {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniqueFd() {
    reset();
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }

  [[nodiscard]] int release() noexcept {
    const auto fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int next = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = next;
  }

private:
  int fd_{-1};
};

[[nodiscard]] core::Error
path_error(std::string message, std::string_view root, std::string_view relative_path, std::string_view reason) {
  return core::Error::permission_denied(std::move(message))
      .with("root", std::string{root})
      .with("path", std::string{relative_path})
      .with("reason", std::string{reason});
}

[[nodiscard]] core::Error
errno_error(std::string message, std::string_view root, std::string_view relative_path, int error_number) {
  auto with_context = [&](core::Error error) {
    return std::move(error)
        .with("root", std::string{root})
        .with("path", std::string{relative_path})
        .with("errno", std::to_string(error_number))
        .with("detail", std::generic_category().message(error_number));
  };

  switch (error_number) {
    case ENOENT:
      return with_context(core::Error::not_found(std::move(message)));
    case EACCES:
    case EPERM:
      return with_context(core::Error::permission_denied(std::move(message)));
    case ELOOP:
      return path_error(std::move(message), root, relative_path, "symlink_component");
    case EXDEV:
      return path_error(std::move(message), root, relative_path, "outside_authority");
    case EAGAIN:
      return core::Error{core::ErrorKind::conflict, std::move(message)}
          .with("root", std::string{root})
          .with("path", std::string{relative_path})
          .with("reason", "path_resolution_raced");
    case ENAMETOOLONG:
    case ENOTDIR:
      return with_context(core::Error::invalid_argument(std::move(message)));
    default:
      return with_context(core::Error::io(std::move(message)));
  }
}

[[nodiscard]] core::Result<void> validate_anchored_path(const AnchoredPath& path) {
  if (path.relative_path.empty()) {
    return std::unexpected(core::Error::invalid_argument("anchored path must not be empty"));
  }
  if (path.relative_path.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("anchored path must not contain NUL"));
  }

  const auto parsed = std::filesystem::path{path.relative_path};
  if (parsed.is_absolute()) {
    return std::unexpected(
        core::Error::invalid_argument("anchored path must be relative").with("path", path.relative_path));
  }
  for (const auto& component : parsed) {
    if (component == "..") {
      return std::unexpected(
          path_error("anchored path must not contain parent traversal", {}, path.relative_path, "outside_authority"));
    }
  }
  return {};
}

[[nodiscard]] std::vector<std::string> path_components(std::string_view relative_path) {
  std::vector<std::string> components;
  for (const auto& component : std::filesystem::path{relative_path}) {
    if (component.empty() || component == ".") {
      continue;
    }
    components.push_back(component.string());
  }
  return components;
}

[[nodiscard]] int duplicate_fd(int fd) noexcept {
#if defined(F_DUPFD_CLOEXEC)
  return ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
  const auto duplicated = ::dup(fd);
  if (duplicated >= 0) {
    static_cast<void>(::fcntl(duplicated, F_SETFD, FD_CLOEXEC));
  }
  return duplicated;
#endif
}

[[nodiscard]] core::Result<UniqueFd>
openat_fallback(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  auto components = path_components(path.relative_path);
  auto current = UniqueFd{duplicate_fd(root_fd)};
  if (current.get() < 0) {
    return std::unexpected(errno_error("failed to duplicate directory authority", root, path.relative_path, errno));
  }

  if (components.empty()) {
    errno = 0;
    const auto opened = ::openat(current.get(), ".", final_flags | O_NOFOLLOW);
    if (opened < 0) {
      return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
    }
    return UniqueFd{opened};
  }

  for (std::size_t index = 0; index + 1U < components.size(); ++index) {
    auto flags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    errno = 0;
    auto next = UniqueFd{::openat(current.get(), components[index].c_str(), flags)};
    if (next.get() < 0) {
      return std::unexpected(errno_error("failed to resolve anchored directory", root, path.relative_path, errno));
    }
    current = std::move(next);
  }

  errno = 0;
  const auto opened = ::openat(current.get(), components.back().c_str(), final_flags | O_NOFOLLOW);
  if (opened < 0) {
    return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
  }
  return UniqueFd{opened};
}

#if defined(__linux__) && defined(SYS_openat2)
[[nodiscard]] core::Result<UniqueFd>
openat2_beneath(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  auto how = open_how{};
  how.flags = static_cast<__u64>(final_flags);
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
  if (path.symlink_policy == AnchoredSymlinkPolicy::reject_all) {
    how.resolve |= RESOLVE_NO_SYMLINKS;
  }

  for (std::size_t attempt = 0; attempt < kResolutionRaceRetries; ++attempt) {
    errno = 0;
    const auto opened =
        static_cast<int>(::syscall(SYS_openat2, root_fd, path.relative_path.c_str(), &how, sizeof(how)));
    if (opened >= 0) {
      return UniqueFd{opened};
    }
    if (errno == ENOSYS) {
      return openat_fallback(root_fd, root, path, final_flags);
    }
    if (errno != EAGAIN || attempt + 1U == kResolutionRaceRetries) {
      return std::unexpected(errno_error("failed to open anchored path", root, path.relative_path, errno));
    }
  }

  return std::unexpected(core::Error::internal("unreachable anchored path retry state"));
}
#endif

[[nodiscard]] core::Result<UniqueFd>
open_beneath(int root_fd, std::string_view root, const AnchoredPath& path, int final_flags) {
  if (auto valid = validate_anchored_path(path); !valid) {
    return std::unexpected(std::move(valid).error());
  }

#if defined(__linux__) && defined(SYS_openat2)
  return openat2_beneath(root_fd, root, path, final_flags);
#else
  return openat_fallback(root_fd, root, path, final_flags);
#endif
}

[[nodiscard]] core::Result<void> require_regular_file(int fd, std::string_view root, std::string_view relative_path) {
  struct stat status{};
  errno = 0;
  if (::fstat(fd, &status) != 0) {
    return std::unexpected(errno_error("failed to inspect anchored file", root, relative_path, errno));
  }
  if (!S_ISREG(status.st_mode)) {
    return std::unexpected(core::Error::invalid_argument("anchored path is not a regular file")
                               .with("root", std::string{root})
                               .with("path", std::string{relative_path}));
  }
  return {};
}

}  // namespace

struct ReadOnlyFile::Impl {
  Impl(UniqueFd opened_fd, std::string opened_path) : fd{std::move(opened_fd)}, display_path{std::move(opened_path)} {}

  UniqueFd fd;
  std::string display_path;
};

struct DirectoryAuthority::Impl {
  Impl(UniqueFd opened_fd, std::string root) : fd{std::move(opened_fd)}, display_root{std::move(root)} {}

  UniqueFd fd;
  std::string display_root;
};

struct FileMutation::Impl {
  UniqueFd parent;
  // Pins the snapshotted inode for the mutation lifetime. Identity validation
  // relies on this descriptor preventing inode-number reuse after unlink.
  UniqueFd existing;
  std::string name;
  std::string display;
  bool existed{false};
  dev_t device{};
  ino_t inode{};
  off_t size{};
  timespec mtime{};
  timespec ctime{};
};

namespace {

[[nodiscard]] bool same_identity(dev_t device,
                                 ino_t inode,
                                 off_t size,
                                 timespec mtime,
                                 timespec ctime,
                                 const struct stat& status) noexcept {
  return status.st_dev == device && status.st_ino == inode && status.st_size == size &&
         status.st_mtim.tv_sec == mtime.tv_sec && status.st_mtim.tv_nsec == mtime.tv_nsec &&
         status.st_ctim.tv_sec == ctime.tv_sec && status.st_ctim.tv_nsec == ctime.tv_nsec;
}

}  // namespace

FileMutation::FileMutation(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
FileMutation::FileMutation(FileMutation&&) noexcept = default;
FileMutation& FileMutation::operator=(FileMutation&&) noexcept = default;
FileMutation::~FileMutation() = default;

std::string_view FileMutation::display_path() const noexcept {
  return impl_ ? std::string_view{impl_->display} : std::string_view{};
}

core::Result<ReadOnlyFile> FileMutation::open_existing() const {
  if (!impl_ || !impl_->existed) {
    auto error = core::Error::not_found("anchored mutation target does not exist");
    if (impl_) {
      error = std::move(error).with("path", impl_->display);
    }
    return std::unexpected(std::move(error));
  }
  errno = 0;
  auto opened =
      UniqueFd{::openat(impl_->parent.get(), impl_->name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (opened.get() < 0) {
    return std::unexpected(errno_error("failed to open mutation target for reading", {}, impl_->display, errno));
  }
  struct stat status{};
  if (::fstat(opened.get(), &status) != 0) {
    return std::unexpected(errno_error("failed to validate mutation read target", {}, impl_->display, errno));
  }
  if (!same_identity(impl_->device, impl_->inode, impl_->size, impl_->mtime, impl_->ctime, status)) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "anchored mutation target changed before read"}
                               .with("reason", "stale_fingerprint")
                               .with("path", impl_->display));
  }
  return ReadOnlyFile{std::make_unique<ReadOnlyFile::Impl>(std::move(opened), impl_->display)};
}

namespace {
core::Result<void> write_all(int fd, std::string_view contents, std::string_view path) {
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto remaining = contents.size() - offset;
    const auto chunk = std::min(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    errno = 0;
    const auto count = ::write(fd, contents.data() + offset, chunk);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(errno_error("failed to write anchored file", {}, path, errno));
    }
    if (count == 0) {
      return std::unexpected(core::Error::io("anchored file write made no progress").with("path", std::string{path}));
    }
    offset += static_cast<std::size_t>(count);
  }
  return {};
}

[[nodiscard]] core::Error stale_mutation_error(std::string message, std::string_view path) {
  return core::Error{core::ErrorKind::conflict, std::move(message)}
      .with("reason", "stale_fingerprint")
      .with("path", std::string{path});
}

[[nodiscard]] core::Result<void> sync_fd(int fd, std::string_view path, std::string message) {
  while (true) {
    errno = 0;
    if (::fsync(fd) == 0) {
      return {};
    }
    if (errno != EINTR) {
      return std::unexpected(errno_error(std::move(message), {}, path, errno));
    }
  }
}

class SiblingTempFile {
public:
  SiblingTempFile(int parent_fd, std::string name, UniqueFd fd)
      : parent_fd_{parent_fd}, name_{std::move(name)}, fd_{std::move(fd)} {}

  SiblingTempFile(const SiblingTempFile&) = delete;
  SiblingTempFile& operator=(const SiblingTempFile&) = delete;
  SiblingTempFile(SiblingTempFile&&) = delete;
  SiblingTempFile& operator=(SiblingTempFile&&) = delete;

  ~SiblingTempFile() {
    if (!name_.empty()) {
      static_cast<void>(::unlinkat(parent_fd_, name_.c_str(), 0));
    }
  }

  [[nodiscard]] int fd() const noexcept {
    return fd_.get();
  }

  [[nodiscard]] const std::string& name() const noexcept {
    return name_;
  }

  void committed() noexcept {
    name_.clear();
  }

private:
  int parent_fd_;
  std::string name_;
  UniqueFd fd_;
};

[[nodiscard]] core::Result<std::unique_ptr<SiblingTempFile>> create_sibling_temp(int parent_fd,
                                                                                 std::string_view display_path) {
  std::random_device random;
  for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
    auto name = std::format(".orangutan.tmp.{}.{}", ::getpid(), random());
    errno = 0;
    auto fd = UniqueFd{::openat(parent_fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666)};
    if (fd.get() >= 0) {
      return std::make_unique<SiblingTempFile>(parent_fd, std::move(name), std::move(fd));
    }
    if (errno != EEXIST) {
      return std::unexpected(errno_error("failed to create anchored temporary file", {}, display_path, errno));
    }
  }
  return std::unexpected(core::Error::io("failed to allocate anchored temporary file")
                             .with("path", std::string{display_path})
                             .with("attempts", "32"));
}

[[nodiscard]] core::Result<void> write_and_sync_temp(SiblingTempFile& temp,
                                                     std::string_view contents,
                                                     std::string_view display_path,
                                                     WriteTextDurability durability) {
  if (auto written = write_all(temp.fd(), contents, display_path); !written) {
    return written;
  }
  if (durability != WriteTextDurability::rename_only) {
    return sync_fd(temp.fd(), display_path, "failed to sync anchored temporary file");
  }
  return {};
}

}  // namespace

core::Result<void> FileMutation::write_text(std::string_view contents, WriteTextOptions options) {
  if (!impl_) {
    return std::unexpected(core::Error::internal("file mutation is empty"));
  }
  if (options.mode != WriteMode::truncate && options.atomic) {
    return std::unexpected(core::Error::invalid_argument("atomic writes require truncate mode"));
  }
  if (!options.atomic && options.durability != WriteTextDurability::rename_only) {
    return std::unexpected(core::Error::invalid_argument("write durability requires atomic mode"));
  }
  if (options.verify_before_commit && (options.mode != WriteMode::truncate || !options.atomic)) {
    return std::unexpected(core::Error::invalid_argument("commit verification requires atomic truncate mode"));
  }
  if (options.verify_before_commit && !impl_->existed) {
    return std::unexpected(core::Error::invalid_argument("commit verification requires an existing target"));
  }

  if (options.mode == WriteMode::append) {
    const auto create_flags = impl_->existed ? 0 : O_CREAT | O_EXCL;
    errno = 0;
    auto opened = UniqueFd{::openat(impl_->parent.get(),
                                    impl_->name.c_str(),
                                    O_WRONLY | O_APPEND | create_flags | O_CLOEXEC | O_NOFOLLOW,
                                    0666)};
    if (opened.get() < 0) {
      if (!impl_->existed && errno == EEXIST) {
        return std::unexpected(stale_mutation_error("anchored append target appeared before write", impl_->display));
      }
      return std::unexpected(errno_error("failed to open anchored append target", {}, impl_->display, errno));
    }
    if (auto regular = require_regular_file(opened.get(), {}, impl_->display); !regular) {
      return regular;
    }
    if (impl_->existed) {
      struct stat opened_status{};
      if (::fstat(opened.get(), &opened_status) != 0) {
        return std::unexpected(errno_error("failed to validate anchored append target", {}, impl_->display, errno));
      }
      if (!same_identity(impl_->device, impl_->inode, impl_->size, impl_->mtime, impl_->ctime, opened_status)) {
        return std::unexpected(stale_mutation_error("anchored append target changed before write", impl_->display));
      }
    }
    return write_all(opened.get(), contents, impl_->display);
  }

  if (options.mode == WriteMode::fail_if_exists) {
    if (impl_->existed) {
      return std::unexpected(
          core::Error{core::ErrorKind::conflict, "file already exists"}.with("path", impl_->display));
    }
    auto temp = create_sibling_temp(impl_->parent.get(), impl_->display);
    if (!temp) {
      return std::unexpected(std::move(temp).error());
    }
    if (auto written = write_and_sync_temp(**temp, contents, impl_->display, options.durability); !written) {
      return written;
    }
    errno = 0;
    if (::linkat(impl_->parent.get(), (*temp)->name().c_str(), impl_->parent.get(), impl_->name.c_str(), 0) != 0) {
      if (errno == EEXIST) {
        return std::unexpected(
            core::Error{core::ErrorKind::conflict, "file already exists"}.with("path", impl_->display));
      }
      return std::unexpected(errno_error("failed to commit anchored exclusive create", {}, impl_->display, errno));
    }
    return {};
  }

  auto temp = create_sibling_temp(impl_->parent.get(), impl_->display);
  if (!temp) {
    return std::unexpected(std::move(temp).error());
  }
  if (auto written = write_and_sync_temp(**temp, contents, impl_->display, options.durability); !written) {
    return written;
  }

  struct stat current{};
  errno = 0;
  const auto current_fd = UniqueFd{::openat(impl_->parent.get(), impl_->name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW)};
  if (impl_->existed) {
    if (current_fd.get() < 0) {
      if (errno == ENOENT) {
        return std::unexpected(stale_mutation_error("anchored mutation target changed before commit", impl_->display));
      }
      return std::unexpected(errno_error("failed to reopen anchored mutation target", {}, impl_->display, errno));
    }
    if (::fstat(current_fd.get(), &current) != 0) {
      return std::unexpected(errno_error("failed to validate anchored mutation target", {}, impl_->display, errno));
    }
    if (!same_identity(impl_->device, impl_->inode, impl_->size, impl_->mtime, impl_->ctime, current)) {
      return std::unexpected(stale_mutation_error("anchored mutation target changed before commit", impl_->display));
    }
    if (options.verify_before_commit) {
      auto verified = options.verify_before_commit(fingerprint_from_stat(current));
      if (!verified) {
        return std::unexpected(std::move(verified).error());
      }
    }
  } else if (current_fd.get() >= 0) {
    return std::unexpected(stale_mutation_error("anchored mutation target appeared before commit", impl_->display));
  } else if (errno != ENOENT) {
    return std::unexpected(errno_error("failed to verify absent anchored mutation target", {}, impl_->display, errno));
  }

  errno = 0;
  if (::renameat(impl_->parent.get(), (*temp)->name().c_str(), impl_->parent.get(), impl_->name.c_str()) != 0) {
    return std::unexpected(errno_error("failed to commit anchored write", {}, impl_->display, errno));
  }
  (*temp)->committed();
  if (options.durability == WriteTextDurability::fsync_file_and_parent) {
    if (auto synced = sync_fd(impl_->parent.get(), impl_->display, "failed to sync anchored parent directory");
        !synced) {
      return synced;
    }
  }
  return {};
}

core::Result<ReadOnlyFile> ReadOnlyFile::open_trusted(std::string_view path) {
  if (path.empty() || path.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("file path must be non-empty and contain no NUL"));
  }

  auto display = std::string{path};
  auto fd = UniqueFd{::open(display.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY)};
  if (fd.get() < 0) {
    const auto open_error = errno;
    if (open_error == ENOTDIR) {
      return std::unexpected(core::Error::not_found("file path does not exist").with("path", display));
    }
    return std::unexpected(errno_error("failed to open file for reading", {}, path, open_error));
  }
  if (auto regular = require_regular_file(fd.get(), {}, path); !regular) {
    return std::unexpected(std::move(regular).error());
  }
  return ReadOnlyFile{std::make_unique<Impl>(std::move(fd), std::move(display))};
}

ReadOnlyFile::ReadOnlyFile(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&&) noexcept = default;

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&&) noexcept = default;

ReadOnlyFile::~ReadOnlyFile() = default;

int ReadOnlyFile::native_handle() const noexcept {
  return impl_ == nullptr ? -1 : impl_->fd.get();
}

std::string_view ReadOnlyFile::display_path() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->display_path};
}

DirectoryAuthority::DirectoryAuthority(std::shared_ptr<const Impl> impl) : impl_{std::move(impl)} {}

core::Result<DirectoryAuthority> DirectoryAuthority::open_trusted(std::string_view root) {
  if (root.empty()) {
    return std::unexpected(core::Error::invalid_argument("directory authority root must not be empty"));
  }
  if (root.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("directory authority root must not contain NUL"));
  }

  const auto root_string = std::string{root};
  errno = 0;
  auto fd = UniqueFd{::open(root_string.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
  if (fd.get() < 0) {
    return std::unexpected(errno_error("failed to open directory authority", root, ".", errno));
  }

  return DirectoryAuthority{std::make_shared<Impl>(std::move(fd), root_string)};
}

core::Result<ReadOnlyFile> DirectoryAuthority::open_file(const AnchoredPath& path) const {
  if (impl_ == nullptr) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }

  auto opened = open_beneath(impl_->fd.get(), impl_->display_root, path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (!opened) {
    return std::unexpected(std::move(opened).error());
  }
  if (auto regular = require_regular_file(opened->get(), impl_->display_root, path.relative_path); !regular) {
    return std::unexpected(std::move(regular).error());
  }
  return ReadOnlyFile{
      std::make_unique<ReadOnlyFile::Impl>(std::move(*opened),
                                           std::format("{}/{}", impl_->display_root, path.relative_path))};
}

core::Result<FileMutation> DirectoryAuthority::begin_file_mutation(std::string_view relative_path,
                                                                   bool create_parent_directories) const {
  if (impl_ == nullptr)
    return std::unexpected(core::Error::internal("directory authority is empty"));
  const auto path = AnchoredPath{
      .relative_path = std::string{relative_path},
      .symlink_policy = AnchoredSymlinkPolicy::reject_all,
  };
  if (auto valid = validate_anchored_path(path); !valid)
    return std::unexpected(std::move(valid).error());

  auto components = path_components(path.relative_path);
  if (components.empty())
    return std::unexpected(core::Error::invalid_argument("mutation target must name a file"));
  auto parent = UniqueFd{::openat(impl_->fd.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (parent.get() < 0)
    return std::unexpected(
        errno_error("failed to duplicate directory authority", impl_->display_root, path.relative_path, errno));

  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    auto next =
        UniqueFd{::openat(parent.get(), components[index].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (next.get() < 0 && errno == ENOENT && create_parent_directories) {
      if (::mkdirat(parent.get(), components[index].c_str(), 0755) != 0 && errno != EEXIST)
        return std::unexpected(
            errno_error("failed to create anchored parent directory", impl_->display_root, path.relative_path, errno));
      next =
          UniqueFd{::openat(parent.get(), components[index].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    }
    if (next.get() < 0) {
      const auto open_error = errno;
      struct stat component_status{};
      if ((open_error == ELOOP || open_error == ENOTDIR) &&
          ::fstatat(parent.get(), components[index].c_str(), &component_status, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISLNK(component_status.st_mode)) {
        return std::unexpected(path_error("anchored mutation parent must not be a symlink",
                                          impl_->display_root,
                                          path.relative_path,
                                          "symlink_component"));
      }
      return std::unexpected(
          errno_error("failed to open anchored parent directory", impl_->display_root, path.relative_path, open_error));
    }
    parent = std::move(next);
  }

  const auto& name = components.back();
  errno = 0;
  auto existing = UniqueFd{::openat(parent.get(), name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW)};
  const bool existed = existing.get() >= 0;
  if (!existed && errno != ENOENT)
    return std::unexpected(
        errno_error("failed to inspect anchored mutation target", impl_->display_root, path.relative_path, errno));
  struct stat status{};
  if (existed) {
    if (::fstat(existing.get(), &status) != 0)
      return std::unexpected(
          errno_error("failed to snapshot anchored mutation target", impl_->display_root, path.relative_path, errno));
    if (S_ISLNK(status.st_mode)) {
      return std::unexpected(path_error("anchored mutation target must not be a symlink",
                                        impl_->display_root,
                                        path.relative_path,
                                        "symlink_component"));
    }
    if (auto regular = require_regular_file(existing.get(), impl_->display_root, path.relative_path); !regular)
      return std::unexpected(std::move(regular).error());
  }
  auto mutation = std::make_unique<FileMutation::Impl>();
  mutation->parent = std::move(parent);
  mutation->existing = std::move(existing);
  mutation->name = name;
  mutation->display = std::format("{}/{}", impl_->display_root, path.relative_path);
  mutation->existed = existed;
  if (existed) {
    mutation->device = status.st_dev;
    mutation->inode = status.st_ino;
    mutation->size = status.st_size;
    mutation->mtime = status.st_mtim;
    mutation->ctime = status.st_ctim;
  }
  return FileMutation{std::move(mutation)};
}

std::string_view DirectoryAuthority::display_root() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->display_root};
}

core::Result<bool> DirectoryAuthority::refers_to_path(std::string_view path) const {
  if (impl_ == nullptr) {
    return std::unexpected(core::Error::internal("directory authority is empty"));
  }
  if (path.empty() || path.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("directory comparison path is invalid"));
  }

  struct stat authority_status{};
  if (::fstat(impl_->fd.get(), &authority_status) != 0) {
    return std::unexpected(errno_error("failed to inspect directory authority", impl_->display_root, ".", errno));
  }

  struct stat path_status{};
  const auto path_string = std::string{path};
  if (::lstat(path_string.c_str(), &path_status) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return false;
    }
    return std::unexpected(errno_error("failed to inspect directory comparison path", path, ".", errno));
  }
  return authority_status.st_dev == path_status.st_dev && authority_status.st_ino == path_status.st_ino;
}

}  // namespace orangutan::io
