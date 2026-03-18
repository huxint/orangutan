#include "infra/subprocess/subprocess.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
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

void signal_process(pid_t pid, int signal_number) {
    if (pid <= 0) {
        return;
    }
    if (killpg(pid, signal_number) == -1) {
        (void)kill(pid, signal_number);
    }
}

[[nodiscard]] std::string make_process_token() {
    static std::atomic<uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return std::to_string(ticks) + "-" + std::to_string(++counter);
}

struct CapturedOutput {
    std::string text;
    size_t total_bytes = 0;
    bool truncated = false;
};

CapturedOutput read_output_tail(const std::filesystem::path &path, size_t max_bytes = 16384) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }

    ifs.seekg(0, std::ios::end);
    const auto end_pos = ifs.tellg();
    if (end_pos <= 0) {
        return {};
    }

    CapturedOutput captured;
    captured.total_bytes = static_cast<size_t>(end_pos);
    const auto start_pos = captured.total_bytes > max_bytes ? static_cast<std::streamoff>(captured.total_bytes - max_bytes) : 0;
    captured.truncated = captured.total_bytes > max_bytes;

    ifs.seekg(start_pos, std::ios::beg);
    captured.text.resize(captured.total_bytes - static_cast<size_t>(start_pos));
    ifs.read(captured.text.data(), static_cast<std::streamsize>(captured.text.size()));
    captured.text.resize(static_cast<size_t>(ifs.gcount()));
    return captured;
}

pid_t spawn_background_subprocess(const SubprocessConfig &config, const std::filesystem::path &stdout_path, const std::filesystem::path &stderr_path) {
    if (!config.stdin_data.empty()) {
        throw std::runtime_error("background subprocesses do not support stdin data");
    }

    pid_t pid = fork();
    if (pid == -1) {
        throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        (void)setpgid(0, 0);

        int devnull = open("/dev/null", O_RDONLY); // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        const int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (stdout_fd < 0) {
            write_child_error("open", stdout_path.string());
            _exit(127);
        }
        const int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (stderr_fd < 0) {
            write_child_error("open", stderr_path.string());
            _exit(127);
        }

        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        close(stdout_fd);
        close(stderr_fd);

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

    return pid;
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

struct BackgroundProcessManager::Impl {
    struct ProcessEntry {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::string process_id;
        std::string command;
        std::string working_dir;
        pid_t pid = -1;
        std::filesystem::path stdout_path;
        std::filesystem::path stderr_path;
        bool running = true;
        bool kill_requested = false;
        std::optional<int> exit_code;
        std::optional<int> signal_number;
    };

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<ProcessEntry>> entries;
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> entries_by_id;
    uint64_t next_id = 0;
    std::filesystem::path temp_root = std::filesystem::temp_directory_path() / ("orangutan-processes-" + make_process_token());

    Impl() {
        std::filesystem::create_directories(temp_root);
    }

    ~Impl() {
        shutdown();
    }

    [[nodiscard]]
    std::shared_ptr<ProcessEntry> find_entry(const std::string &process_id) const {
        std::scoped_lock lock(mutex);
        const auto it = entries_by_id.find(process_id);
        if (it == entries_by_id.end()) {
            return nullptr;
        }
        return it->second;
    }

    [[nodiscard]]
    BackgroundProcessSummary summarize_entry(const std::shared_ptr<ProcessEntry> &entry) const {
        BackgroundProcessSummary summary;
        {
            std::scoped_lock lock(entry->mutex);
            summary.process_id = entry->process_id;
            summary.command = entry->command;
            summary.working_dir = entry->working_dir;
            summary.pid = static_cast<int>(entry->pid);
            summary.running = entry->running;
            summary.kill_requested = entry->kill_requested;
            summary.exit_code = entry->exit_code;
            summary.signal_number = entry->signal_number;
        }

        std::error_code ec;
        summary.stdout_bytes = std::filesystem::exists(entry->stdout_path, ec) && !ec ? std::filesystem::file_size(entry->stdout_path, ec) : 0;
        ec.clear();
        summary.stderr_bytes = std::filesystem::exists(entry->stderr_path, ec) && !ec ? std::filesystem::file_size(entry->stderr_path, ec) : 0;
        return summary;
    }

    [[nodiscard]]
    BackgroundProcessSnapshot snapshot_entry(const std::shared_ptr<ProcessEntry> &entry) const {
        auto snapshot = BackgroundProcessSnapshot{summarize_entry(entry)};
        const auto stdout_capture = read_output_tail(entry->stdout_path);
        snapshot.stdout_output = std::move(stdout_capture.text);
        snapshot.stdout_truncated = stdout_capture.truncated;
        snapshot.stdout_bytes = stdout_capture.total_bytes;

        const auto stderr_capture = read_output_tail(entry->stderr_path);
        snapshot.stderr_output = std::move(stderr_capture.text);
        snapshot.stderr_truncated = stderr_capture.truncated;
        snapshot.stderr_bytes = stderr_capture.total_bytes;
        return snapshot;
    }

    [[nodiscard]]
    BackgroundProcessSnapshot terminate_entry(const std::shared_ptr<ProcessEntry> &entry) {
        pid_t pid = -1;
        bool already_stopped = false;
        {
            std::scoped_lock lock(entry->mutex);
            if (!entry->running) {
                already_stopped = true;
            } else {
                entry->kill_requested = true;
                pid = entry->pid;
            }
        }
        if (already_stopped) {
            return snapshot_entry(entry);
        }

        signal_process(pid, SIGTERM);

        std::unique_lock lock(entry->mutex);
        if (!entry->cv.wait_for(lock, std::chrono::milliseconds(500), [&entry] {
                return !entry->running;
            })) {
            lock.unlock();
            signal_process(pid, SIGKILL);
            lock.lock();
            (void)entry->cv.wait_for(lock, std::chrono::seconds(2), [&entry] {
                return !entry->running;
            });
        }
        lock.unlock();
        return snapshot_entry(entry);
    }

    void shutdown() {
        std::vector<std::shared_ptr<ProcessEntry>> active_entries;
        {
            std::scoped_lock lock(mutex);
            active_entries = entries;
        }

        for (const auto &entry : active_entries) {
            (void)terminate_entry(entry);
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }
};

BackgroundProcessManager::BackgroundProcessManager()
: impl_(std::make_unique<Impl>()) {}

BackgroundProcessManager::~BackgroundProcessManager() = default;

BackgroundProcessManager::BackgroundProcessManager(BackgroundProcessManager &&) noexcept = default;

BackgroundProcessManager &BackgroundProcessManager::operator=(BackgroundProcessManager &&) noexcept = default;

BackgroundProcessSummary BackgroundProcessManager::start(const SubprocessConfig &config, const std::string &display_command) {
    auto entry = std::make_shared<Impl::ProcessEntry>();
    {
        std::scoped_lock lock(impl_->mutex);
        entry->process_id = "proc-" + std::to_string(++impl_->next_id);
    }
    entry->command = display_command.empty() ? config.command : display_command;
    entry->working_dir = config.working_dir;
    entry->stdout_path = impl_->temp_root / (entry->process_id + ".stdout");
    entry->stderr_path = impl_->temp_root / (entry->process_id + ".stderr");

    std::ofstream(entry->stdout_path).close();
    std::ofstream(entry->stderr_path).close();

    try {
        entry->pid = spawn_background_subprocess(config, entry->stdout_path, entry->stderr_path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(entry->stdout_path, ec);
        std::filesystem::remove(entry->stderr_path, ec);
        throw;
    }

    std::thread([entry]() {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(entry->pid, &status, 0);
        } while (waited == -1 && errno == EINTR);

        {
            std::scoped_lock lock(entry->mutex);
            entry->running = false;
            if (waited == -1) {
                entry->exit_code = -1;
            } else if (WIFEXITED(status)) {
                entry->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                entry->signal_number = WTERMSIG(status);
                entry->exit_code = 128 + WTERMSIG(status);
            } else {
                entry->exit_code = -1;
            }
        }
        entry->cv.notify_all();
    }).detach();

    {
        std::scoped_lock lock(impl_->mutex);
        impl_->entries.push_back(entry);
        impl_->entries_by_id.insert_or_assign(entry->process_id, entry);
    }

    return impl_->summarize_entry(entry);
}

std::vector<BackgroundProcessSummary> BackgroundProcessManager::list() const {
    std::vector<std::shared_ptr<Impl::ProcessEntry>> entries;
    {
        std::scoped_lock lock(impl_->mutex);
        entries = impl_->entries;
    }

    std::vector<BackgroundProcessSummary> summaries;
    summaries.reserve(entries.size());
    for (const auto &entry : entries) {
        summaries.push_back(impl_->summarize_entry(entry));
    }
    return summaries;
}

BackgroundProcessSnapshot BackgroundProcessManager::poll(const std::string &process_id) const {
    const auto entry = impl_->find_entry(process_id);
    if (entry == nullptr) {
        throw std::runtime_error("background process not found: " + process_id);
    }
    return impl_->snapshot_entry(entry);
}

BackgroundProcessSnapshot BackgroundProcessManager::kill(const std::string &process_id) {
    const auto entry = impl_->find_entry(process_id);
    if (entry == nullptr) {
        throw std::runtime_error("background process not found: " + process_id);
    }
    return impl_->terminate_entry(entry);
}

} // namespace orangutan
