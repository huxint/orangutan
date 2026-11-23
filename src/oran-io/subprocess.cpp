#include <oran/io/subprocess.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <oran/core/error.hpp>
#include <oran/io/blocking.hpp>

namespace orangutan::io {
namespace {

constexpr std::size_t kMaxCommandBytes = 32U * 1024U;
constexpr std::size_t kMinOutputBytes = 256U;
constexpr std::size_t kMaxOutputBytes = 16U * 1024U * 1024U;
constexpr auto kMaxTimeout = std::chrono::milliseconds{300'000};
constexpr auto kKillGrace = std::chrono::milliseconds{150};

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
  [[nodiscard]] bool valid() const noexcept {
    return fd_ >= 0;
  }
  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
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

[[nodiscard]] core::Error errno_error(std::string message, int err) {
  return core::Error::io(std::move(message))
      .with("errno", std::to_string(err))
      .with("detail", std::generic_category().message(err));
}

[[nodiscard]] core::Result<std::pair<UniqueFd, UniqueFd>> make_pipe() {
  std::array<int, 2> fds{-1, -1};
  if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
    return std::unexpected(errno_error("failed to create subprocess pipe", errno));
  }
  return std::pair{UniqueFd{fds[0]}, UniqueFd{fds[1]}};
}

[[nodiscard]] core::Result<void> set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return std::unexpected(errno_error("failed to read pipe flags", errno));
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return std::unexpected(errno_error("failed to mark pipe non-blocking", errno));
  }
  return {};
}

void trim_trailing_incomplete_utf8(std::string& text) noexcept {
  if (text.empty()) {
    return;
  }
  std::size_t back = text.size();
  std::size_t cont = 0;
  while (back > 0 && cont < 3U && (static_cast<std::uint8_t>(text[back - 1]) & 0xC0U) == 0x80U) {
    --back;
    ++cont;
  }
  if (back == 0) {
    text.clear();
    return;
  }
  const auto lead = static_cast<std::uint8_t>(text[back - 1]);
  std::size_t expected = 1U;
  if (lead < 0x80U) {
    expected = 1U;
  } else if ((lead & 0xE0U) == 0xC0U) {
    expected = 2U;
  } else if ((lead & 0xF0U) == 0xE0U) {
    expected = 3U;
  } else if ((lead & 0xF8U) == 0xF0U) {
    expected = 4U;
  } else {
    text.resize(back - 1);
    return;
  }
  if (cont + 1U < expected) {
    text.resize(back - 1);
  }
}

void drain_pipe(UniqueFd& fd, std::string& out, bool& truncated, std::size_t max_bytes) {
  if (!fd.valid()) {
    return;
  }
  std::array<char, 4096> buffer{};
  while (true) {
    const auto n = ::read(fd.get(), buffer.data(), buffer.size());
    if (n > 0) {
      const auto count = static_cast<std::size_t>(n);
      if (out.size() < max_bytes) {
        const auto room = max_bytes - out.size();
        const auto take = std::min(room, count);
        out.append(buffer.data(), take);
        if (take < count) {
          truncated = true;
        }
      } else {
        truncated = true;
      }
      continue;
    }
    if (n == 0) {
      fd.reset();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    fd.reset();
    return;
  }
}

void terminate_process_group(pid_t pid, bool& reaped, int& raw_status) noexcept {
  if (pid <= 0 || reaped) {
    return;
  }
  static_cast<void>(::kill(-pid, SIGTERM));
  const auto grace_end = std::chrono::steady_clock::now() + kKillGrace;
  while (std::chrono::steady_clock::now() < grace_end) {
    const pid_t w = ::waitpid(pid, &raw_status, WNOHANG);
    if (w == pid || (w < 0 && errno == ECHILD)) {
      reaped = true;
      return;
    }
    ::poll(nullptr, 0, 15);
  }
  static_cast<void>(::kill(-pid, SIGKILL));
  while (true) {
    const pid_t w = ::waitpid(pid, &raw_status, 0);
    if (w == pid || (w < 0 && errno != EINTR)) {
      reaped = true;
      return;
    }
  }
}

[[nodiscard]] int decode_exit_status(int status) noexcept {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

[[nodiscard]] core::Result<void> validate_options(const SubprocessOptions& options) {
  if (options.command.empty()) {
    return std::unexpected(core::Error::invalid_argument("subprocess command must not be empty"));
  }
  if (options.command.size() > kMaxCommandBytes) {
    return std::unexpected(core::Error::invalid_argument("subprocess command exceeds byte limit"));
  }
  if (options.command.contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("subprocess command must not contain NUL bytes"));
  }
  if (options.timeout <= std::chrono::milliseconds{0} || options.timeout > kMaxTimeout) {
    return std::unexpected(core::Error::invalid_argument("subprocess timeout is out of range"));
  }
  if (options.max_output_bytes < kMinOutputBytes || options.max_output_bytes > kMaxOutputBytes) {
    return std::unexpected(core::Error::invalid_argument("subprocess max_output_bytes is out of range"));
  }
  if (!options.working_directory.empty()) {
    if (options.working_directory.contains('\0')) {
      return std::unexpected(core::Error::invalid_argument("subprocess working_directory contains NUL"));
    }
    std::error_code ec;
    const auto status = std::filesystem::status(std::filesystem::path{options.working_directory}, ec);
    if (ec || !std::filesystem::exists(status) || !std::filesystem::is_directory(status)) {
      return std::unexpected(core::Error::invalid_argument("subprocess working_directory must be an existing directory")
                                 .with("working_directory", options.working_directory));
    }
  }
  return {};
}

[[nodiscard]] core::Result<SubprocessResult> execute_blocking(const SubprocessOptions& options, std::stop_token stop) {
  auto stdout_pipe = make_pipe();
  if (!stdout_pipe) {
    return std::unexpected(std::move(stdout_pipe).error());
  }
  auto stderr_pipe = make_pipe();
  if (!stderr_pipe) {
    return std::unexpected(std::move(stderr_pipe).error());
  }

  const auto started = std::chrono::steady_clock::now();
  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(errno_error("failed to fork subprocess", errno));
  }

  if (pid == 0) {
    static_cast<void>(::setpgid(0, 0));
    const int dev_null = ::open("/dev/null", O_RDONLY);
    if (dev_null >= 0) {
      static_cast<void>(::dup2(dev_null, STDIN_FILENO));
      if (dev_null > STDERR_FILENO) {
        static_cast<void>(::close(dev_null));
      }
    }
    static_cast<void>(::dup2(stdout_pipe->second.get(), STDOUT_FILENO));
    static_cast<void>(::dup2(stderr_pipe->second.get(), STDERR_FILENO));
    stdout_pipe->first.reset();
    stdout_pipe->second.reset();
    stderr_pipe->first.reset();
    stderr_pipe->second.reset();

    if (!options.working_directory.empty()) {
      if (::chdir(options.working_directory.c_str()) != 0) {
        ::_exit(126);
      }
    }
    ::execl("/bin/sh", "sh", "-c", options.command.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }

  static_cast<void>(::setpgid(pid, pid));
  stdout_pipe->second.reset();
  stderr_pipe->second.reset();

  if (auto nb = set_nonblocking(stdout_pipe->first.get()); !nb) {
    bool reaped = false;
    int status = 0;
    terminate_process_group(pid, reaped, status);
    return std::unexpected(std::move(nb).error());
  }
  if (auto nb = set_nonblocking(stderr_pipe->first.get()); !nb) {
    bool reaped = false;
    int status = 0;
    terminate_process_group(pid, reaped, status);
    return std::unexpected(std::move(nb).error());
  }

  SubprocessResult result;
  const auto deadline = started + options.timeout;
  bool reaped = false;
  int raw_status = 0;

  while (stdout_pipe->first.valid() || stderr_pipe->first.valid() || !reaped) {
    if (stop.stop_requested()) {
      terminate_process_group(pid, reaped, raw_status);
      return std::unexpected(core::Error::cancelled().with("phase", "subprocess"));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      terminate_process_group(pid, reaped, raw_status);
      drain_pipe(stdout_pipe->first, result.stdout_text, result.stdout_truncated, options.max_output_bytes);
      drain_pipe(stderr_pipe->first, result.stderr_text, result.stderr_truncated, options.max_output_bytes);
      break;
    }

    std::array<pollfd, 2> pfds{};
    nfds_t count = 0;
    if (stdout_pipe->first.valid()) {
      pfds[count++] = pollfd{.fd = stdout_pipe->first.get(), .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stderr_pipe->first.valid()) {
      pfds[count++] = pollfd{.fd = stderr_pipe->first.get(), .events = POLLIN | POLLHUP, .revents = 0};
    }

    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int wait_ms = static_cast<int>(std::clamp<long long>(remaining_ms, 1, 25));
    if (count > 0) {
      const int ready = ::poll(pfds.data(), count, wait_ms);
      if (ready > 0) {
        drain_pipe(stdout_pipe->first, result.stdout_text, result.stdout_truncated, options.max_output_bytes);
        drain_pipe(stderr_pipe->first, result.stderr_text, result.stderr_truncated, options.max_output_bytes);
      }
    } else {
      ::poll(nullptr, 0, wait_ms);
    }

    if (!reaped) {
      const pid_t w = ::waitpid(pid, &raw_status, WNOHANG);
      if (w == pid) {
        reaped = true;
        drain_pipe(stdout_pipe->first, result.stdout_text, result.stdout_truncated, options.max_output_bytes);
        drain_pipe(stderr_pipe->first, result.stderr_text, result.stderr_truncated, options.max_output_bytes);
      } else if (w < 0 && errno != EINTR) {
        reaped = true;
      }
    }
  }

  if (result.stdout_truncated) {
    trim_trailing_incomplete_utf8(result.stdout_text);
  }
  if (result.stderr_truncated) {
    trim_trailing_incomplete_utf8(result.stderr_text);
  }
  result.exit_code = result.timed_out ? -1 : decode_exit_status(raw_status);
  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  return result;
}

}  // namespace

async::Awaitable<core::Result<SubprocessResult>>
run_subprocess(asio::any_io_executor executor, SubprocessOptions options) {
  if (auto valid = validate_options(options); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  co_return co_await run_blocking(
      std::move(executor),
      [options = std::move(options)](std::stop_token stop) { return execute_blocking(options, stop); });
}

}  // namespace orangutan::io
