#include "infra/subprocess/subprocess.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace orangutan {

namespace {

void write_all(int fd, std::string_view text) {
    size_t written = 0;
    while (written < text.size()) {
        const auto pending = text.substr(written);
        const auto count = write(fd, pending.data(), pending.size());
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

void write_child_error(std::string_view operation, std::string_view path) {
    std::string message(operation);
    message += '(';
    message += path;
    message += ") failed: ";
    message += std::strerror(errno);
    message += '\n';
    write_all(STDERR_FILENO, message);
}

// Calculate remaining milliseconds until deadline, returns 0 if expired.
int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(ms);
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1; // NOLINT(cppcoreguidelines-pro-type-vararg)
}

bool drain_fd(int fd, std::string &output) {
    std::array<char, 4096> buffer{};
    while (true) {
        auto n = read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            output.append(buffer.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

enum class WriteStatus {
    open,
    closed,
    done,
};

WriteStatus write_pending(int fd, std::string_view data, size_t &written) {
    while (written < data.size()) {
        const auto pending = data.substr(written);
        auto n = write(fd, pending.data(), pending.size());
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return WriteStatus::open;
        }
        return WriteStatus::closed;
    }
    return WriteStatus::done;
}

} // namespace

SubprocessResult run_subprocess(const SubprocessConfig &config) {
    // Create pipes: stdin (parent writes), stdout (parent reads), stderr (parent reads)
    std::array<int, 2> stdin_pipe{};
    std::array<int, 2> stdout_pipe{};
    std::array<int, 2> stderr_pipe{};

    bool need_stdin_pipe = !config.stdin_data.empty();

    if (need_stdin_pipe && pipe(stdin_pipe.data()) == -1) {
        return {.exit_code = -1, .stderr_output = "pipe() failed: " + std::string(std::strerror(errno))};
    }
    if (pipe(stdout_pipe.data()) == -1 || pipe(stderr_pipe.data()) == -1) {
        if (need_stdin_pipe) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        return {.exit_code = -1, .stderr_output = "pipe() failed: " + std::string(std::strerror(errno))};
    }

    pid_t pid = fork();
    if (pid == -1) {
        if (need_stdin_pipe) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        for (auto fd : {stdout_pipe[0], stdout_pipe[1], stderr_pipe[0], stderr_pipe[1]}) {
            close(fd);
        }
        return {.exit_code = -1, .stderr_output = "fork() failed: " + std::string(std::strerror(errno))};
    }

    if (pid == 0) {
        // Child: create own process group for clean kill (skip if not possible)
        (void)setpgid(0, 0);

        // Redirect stdio
        if (need_stdin_pipe) {
            close(stdin_pipe[1]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        } else {
            // No stdin data: redirect from /dev/null so child doesn't inherit parent's stdin
            int devnull = open("/dev/null", O_RDONLY); // NOLINT(cppcoreguidelines-pro-type-vararg)
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (!config.working_dir.empty() && chdir(config.working_dir.c_str()) != 0) {
            write_child_error("chdir", config.working_dir);
            _exit(127);
        }

        if (config.use_shell) {
            auto shell_command = config.command;
            auto shell_name = std::to_array("sh");
            auto shell_flag = std::to_array("-c");
            std::array<char *, 4> shell_argv = {
                shell_name.data(),
                shell_flag.data(),
                shell_command.data(),
                nullptr,
            };
            execv("/bin/sh", shell_argv.data());
        }

        auto exec_path = config.command;
        auto argv0 = std::filesystem::path(config.command).filename().string();
        if (argv0.empty()) {
            argv0 = exec_path;
        }
        std::array<char *, 2> argv = {
            argv0.data(),
            nullptr,
        };
        execv(exec_path.c_str(), argv.data());
        write_child_error("exec", config.command);
        _exit(127);
    }

    // Parent: close child-side pipe ends
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Write stdin data to child and close pipe
    if (need_stdin_pipe) {
        close(stdin_pipe[0]);
        if (!set_nonblocking(stdin_pipe[1])) {
            (void)kill(pid, SIGKILL);
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            waitpid(pid, nullptr, 0);
            return {.exit_code = -1, .stderr_output = "fcntl() failed: " + std::string(std::strerror(errno))};
        }
    }

    if (!set_nonblocking(stdout_pipe[0]) || !set_nonblocking(stderr_pipe[0])) {
        (void)kill(pid, SIGKILL);
        if (need_stdin_pipe) {
            close(stdin_pipe[1]);
        }
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        waitpid(pid, nullptr, 0);
        return {.exit_code = -1, .stderr_output = "fcntl() failed: " + std::string(std::strerror(errno))};
    }

    // Poll-based stdin/stdout/stderr loop with deadline
    auto deadline = std::chrono::steady_clock::now() + config.timeout;
    SubprocessResult result;
    size_t stdin_written = 0;
    int stdin_fd = need_stdin_pipe ? stdin_pipe[1] : -1;
    int stdout_fd = stdout_pipe[0];
    int stderr_fd = stderr_pipe[0];
    bool child_exited = false;
    int status = 0;
    bool fatal_error = false;

    while (stdin_fd != -1 || stdout_fd != -1 || stderr_fd != -1 || !child_exited) {
        if (!child_exited) {
            auto wp = waitpid(pid, &status, WNOHANG);
            if (wp == pid) {
                child_exited = true;
            } else if (wp < 0 && errno != EINTR) {
                fatal_error = true;
                break;
            }
        }

        int timeout_ms = remaining_ms(deadline);
        if (timeout_ms <= 0) {
            result.timed_out = true;
            break;
        }

        std::array<struct pollfd, 3> fds{};
        fds[0] = {.fd = stdin_fd, .events = static_cast<short>(stdin_fd != -1 ? POLLOUT : 0), .revents = 0};
        fds[1] = {.fd = stdout_fd, .events = static_cast<short>(stdout_fd != -1 ? POLLIN : 0), .revents = 0};
        fds[2] = {.fd = stderr_fd, .events = static_cast<short>(stderr_fd != -1 ? POLLIN : 0), .revents = 0};

        bool has_active_fds = (stdin_fd != -1 || stdout_fd != -1 || stderr_fd != -1);
        int poll_timeout_ms = has_active_fds ? timeout_ms : std::min(timeout_ms, 10);

        int ready = poll(fds.data(), fds.size(), poll_timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fatal_error = true;
            break;
        }
        if (ready == 0) {
            if (!has_active_fds) {
                continue;
            }
            result.timed_out = true;
            break;
        }

        if (stdin_fd != -1) {
            if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                close(stdin_fd);
                stdin_fd = -1;
            } else if ((fds[0].revents & POLLOUT) != 0) {
                auto write_status = write_pending(stdin_fd, config.stdin_data, stdin_written);
                if (write_status != WriteStatus::open) {
                    close(stdin_fd);
                    stdin_fd = -1;
                }
            }
        }

        if (stdout_fd != -1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if (!drain_fd(stdout_fd, result.stdout_output)) {
                close(stdout_fd);
                stdout_fd = -1;
            }
        }

        if (stderr_fd != -1 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if (!drain_fd(stderr_fd, result.stderr_output)) {
                close(stderr_fd);
                stderr_fd = -1;
            }
        }
    }

    if (stdin_fd != -1) {
        close(stdin_fd);
    }
    if (stdout_fd != -1) {
        close(stdout_fd);
    }
    if (stderr_fd != -1) {
        close(stderr_fd);
    }

    // Kill and reap
    if (result.timed_out || fatal_error) {
        if (!child_exited) {
            if (killpg(pid, SIGKILL) == -1 && errno == ESRCH) {
                (void)kill(pid, SIGKILL);
            }
        }
        waitpid(pid, nullptr, 0);
        result.exit_code = -1;
        return result;
    }

    if (!child_exited) {
        waitpid(pid, &status, 0);
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

} // namespace orangutan
