#include <oran/io/private_directory.hpp>

#include <array>
#include <cerrno>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <oran/core/turn_id.hpp>

namespace orangutan::io {
namespace {

class Descriptor {
public:
  explicit Descriptor(int value) : value_{value} {}
  ~Descriptor() {
    if (value_ >= 0)
      ::close(value_);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const noexcept {
    return value_;
  }
  int release() noexcept {
    return std::exchange(value_, -1);
  }

private:
  int value_;
};

core::Error failure(std::string message) {
  return core::Error::io(std::move(message)).with("cause", std::error_code{errno, std::generic_category()}.message());
}

bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= 128 && name != "." && name != ".." && !name.contains('/') &&
         !name.contains('\0');
}

core::Result<void> check_file(int descriptor) {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0)
    return std::unexpected(failure("cannot inspect private file"));
  if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 ||
      status.st_nlink != 1) {
    return std::unexpected(core::Error::permission_denied("application file must be private and owned"));
  }
  return {};
}

}  // namespace

FileLock::FileLock(int descriptor) noexcept : descriptor_{descriptor} {}
FileLock::~FileLock() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
}
FileLock::FileLock(FileLock&& other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)} {}
FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0)
      ::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}

PrivateDirectory::PrivateDirectory(int descriptor) noexcept : descriptor_{descriptor} {}
PrivateDirectory::~PrivateDirectory() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
}
PrivateDirectory::PrivateDirectory(PrivateDirectory&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)} {}
PrivateDirectory& PrivateDirectory::operator=(PrivateDirectory&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0)
      ::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}

core::Result<PrivateDirectory> PrivateDirectory::open(std::string path) {
  if (path.empty() || path.contains('\0') || !std::filesystem::path{path}.is_absolute()) {
    return std::unexpected(core::Error::invalid_argument("application directory must be an absolute path"));
  }
  std::error_code error;
  const bool created = std::filesystem::create_directories(path, error);
  if (error)
    return std::unexpected(core::Error::io("cannot create application directory").with("cause", error.message()));
  Descriptor descriptor{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0)
    return std::unexpected(failure("cannot open application directory"));
  if (created && ::fchmod(descriptor.get(), 0700) != 0)
    return std::unexpected(failure("cannot protect application directory"));
  struct stat status{};
  if (::fstat(descriptor.get(), &status) != 0)
    return std::unexpected(failure("cannot inspect application directory"));
  if (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
    return std::unexpected(core::Error::permission_denied("application directory must be private to its owner"));
  }
  return PrivateDirectory{descriptor.release()};
}

core::Result<std::optional<std::string>> PrivateDirectory::read(std::string_view name, std::size_t max_bytes) const {
  if (!valid_name(name) || max_bytes == 0)
    return std::unexpected(core::Error::invalid_argument("invalid private file read"));
  const std::string filename{name};
  Descriptor file{::openat(descriptor_, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (file.get() < 0) {
    if (errno == ENOENT)
      return std::nullopt;
    return std::unexpected(failure("cannot read private application file"));
  }
  if (auto checked = check_file(file.get()); !checked)
    return std::unexpected(std::move(checked).error());
  std::string contents;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return std::unexpected(failure("cannot read private application file"));
    }
    if (count == 0)
      return contents;
    const auto size = static_cast<std::size_t>(count);
    if (size > max_bytes - contents.size())
      return std::unexpected(core::Error::invalid_argument("application file exceeds byte limit"));
    contents.append(buffer.data(), size);
  }
}

core::Result<void> PrivateDirectory::write(std::string_view name, std::string_view contents) const {
  if (!valid_name(name))
    return std::unexpected(core::Error::invalid_argument("invalid private file name"));
  auto id = core::generate_turn_id();
  if (!id)
    return std::unexpected(std::move(id).error());
  const auto temporary = ".write-" + core::format_turn_id_hex(*id);
  const std::string filename{name};
  Descriptor file{::openat(descriptor_, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (file.get() < 0)
    return std::unexpected(failure("cannot create private application file"));
  core::Result<void> result;
  for (std::size_t offset = 0; offset < contents.size();) {
    const auto count = ::write(file.get(), contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      result = std::unexpected(failure("cannot write application file"));
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (result && ::fsync(file.get()) != 0)
    result = std::unexpected(failure("cannot sync application file"));
  if (result && ::renameat(descriptor_, temporary.c_str(), descriptor_, filename.c_str()) != 0) {
    result = std::unexpected(failure("cannot replace application file"));
  }
  if (!result) {
    ::unlinkat(descriptor_, temporary.c_str(), 0);
    return result;
  }
  if (::fsync(descriptor_) != 0)
    return std::unexpected(failure("cannot sync application directory"));
  return {};
}

core::Result<FileLock> PrivateDirectory::lock(std::string_view name) const {
  if (!valid_name(name))
    return std::unexpected(core::Error::invalid_argument("invalid application lock name"));
  const std::string filename{name};
  Descriptor file{
      ::openat(descriptor_, filename.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
  if (file.get() < 0)
    return std::unexpected(failure("cannot open application lock"));
  if (auto checked = check_file(file.get()); !checked)
    return std::unexpected(std::move(checked).error());
  if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK)
      return std::unexpected(core::Error{core::ErrorKind::conflict, "application state is already owned"});
    return std::unexpected(failure("cannot acquire application lock"));
  }
  return FileLock{file.release()};
}

}  // namespace orangutan::io
