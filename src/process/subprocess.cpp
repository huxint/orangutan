#include "process/subprocess.hpp"
#include "process/posix-fd-utils.hpp"
#include "types/base.hpp"
#include "utils/format.hpp"
#include "utils/sender-utils.hpp"
#include "utils/file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spdlog/common.h>
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

namespace orangutan::process {

    namespace {

        bool drain_fd(int fd, std::string &output) {
            std::array<char, 4096> buffer{};
            while (true) {
                auto n = read(fd, buffer.data(), buffer.size());
                if (n > 0) {
                    output.append(buffer.data(), static_cast<std::size_t>(n));
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

        enum class write_status : base::u8 {
            open,
            closed,
            done,
        };

        write_status write_pending(int fd, std::string_view data, std::size_t &written) {
            while (written < data.size()) {
                const auto pending = data.substr(written);
                auto n = write(fd, pending.data(), pending.size());
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return write_status::open;
                }
                return write_status::closed;
            }
            return write_status::done;
        }

        void signal_process(pid_t pid, int signal_number) {
            if (pid <= 0) {
                return;
            }
            if (killpg(pid, signal_number) == -1) {
                static_cast<void>(kill(pid, signal_number));
            }
        }

        void reset_child_signal_state_for_exec() {
            sigset_t empty_mask{};
            sigemptyset(&empty_mask);
            static_cast<void>(sigprocmask(SIG_SETMASK, &empty_mask, nullptr));

            constexpr std::array<int, 9> RESET_SIGNALS = {
                SIGINT, SIGTERM, SIGQUIT, SIGPIPE, SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGSEGV,
            };
            for (const auto signal_number : RESET_SIGNALS) {
                struct sigaction action{};
                action.sa_handler = SIG_DFL;
                sigemptyset(&action.sa_mask);
                static_cast<void>(sigaction(signal_number, &action, nullptr));
            }
        }

        [[nodiscard]]
        std::string make_process_token() {
            static std::atomic<base::u64> counter{0};
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            return utils::format("{}-{}", ticks, ++counter);
        }

        struct CapturedOutput {
            std::string text;
            std::size_t total_bytes = 0;
            bool truncated = false;
        };

        CapturedOutput read_output_tail(const std::filesystem::path &path, std::size_t max_bytes = 16384) {
            try {
                fileio::File file(path, "rb");

                std::error_code ec;
                const auto total_bytes = std::filesystem::file_size(path, ec);
                if (ec != std::error_code{} || total_bytes == 0) {
                    return {};
                }

                CapturedOutput captured;
                captured.total_bytes = static_cast<std::size_t>(total_bytes);
                captured.truncated = captured.total_bytes > max_bytes;

                if (captured.truncated) {
                    if (std::fseek(file.get(), -static_cast<long>(max_bytes), SEEK_END) != 0) {
                        return {};
                    }
                }

                const auto read_limit = captured.truncated ? max_bytes : captured.total_bytes;
                bool read_error = false;
                captured.text.resize_and_overwrite(read_limit, [&file, &read_error](char *buffer, std::size_t size) {
                    const auto bytes_read = std::fread(buffer, sizeof(char), size, file.get());
                    read_error = bytes_read == 0 && size > 0 && std::ferror(file.get()) != 0;
                    return bytes_read;
                });
                if (read_error) {
                    return {};
                }
                file.close();
                return captured;
            } catch (const std::runtime_error &) {
                return {};
            }
        }

        BackgroundProcessOutputMetadata to_output_metadata(CapturedOutput captured) {
            return {
                .tail = std::move(captured.text),
                .total_bytes = captured.total_bytes,
                .truncated = captured.truncated,
            };
        }

        std::size_t file_size_or_zero(const std::filesystem::path &path) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) {
                return 0;
            }
            return static_cast<std::size_t>(size);
        }

        pid_t spawn_background_subprocess(const SubprocessConfig &config, const std::filesystem::path &stdout_path, const std::filesystem::path &stderr_path) {
            if (!config.stdin_data.empty()) {
                throw std::runtime_error("background subprocesses do not support stdin data");
            }

            std::array<int, 2> ready_pipe{};
            if (pipe(ready_pipe.data()) == -1) {
                throw std::runtime_error("pipe() failed: " + std::string(std::strerror(errno)));
            }

            pid_t pid = fork();
            if (pid == -1) {
                close(ready_pipe[0]);
                close(ready_pipe[1]);
                throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
            }

            if (pid == 0) {
                close(ready_pipe[0]);
                reset_child_signal_state_for_exec();
                static_cast<void>(setpgid(0, 0));
                static_cast<void>(write(ready_pipe[1], "1", 1));
                close(ready_pipe[1]);

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

            close(ready_pipe[1]);
            char ready = 0;
            auto ready_read = read(ready_pipe[0], &ready, 1);
            while (ready_read == -1 && errno == EINTR) {
                ready_read = read(ready_pipe[0], &ready, 1);
            }
            close(ready_pipe[0]);
            if (ready_read != 1) {
                signal_process(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {
                }
                throw std::runtime_error("background subprocess failed to initialize");
            }

            static_cast<void>(setpgid(pid, pid));
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
            static_cast<void>(setpgid(0, 0));

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

        static_cast<void>(setpgid(pid, pid));

        // Parent: close child-side pipe ends
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Write stdin data to child and close pipe
        if (need_stdin_pipe) {
            close(stdin_pipe[0]);
            if (!set_nonblocking(stdin_pipe[1])) {
                signal_process(pid, SIGKILL);
                close(stdin_pipe[1]);
                close(stdout_pipe[0]);
                close(stderr_pipe[0]);
                waitpid(pid, nullptr, 0);
                return {.exit_code = -1, .stderr_output = "fcntl() failed: " + std::string(std::strerror(errno))};
            }
        }

        if (!set_nonblocking(stdout_pipe[0]) || !set_nonblocking(stderr_pipe[0])) {
            signal_process(pid, SIGKILL);
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
        std::size_t stdin_written = 0;
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
                    if (write_status != write_status::open) {
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
                signal_process(pid, SIGKILL);
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
            BackgroundProcessCompletionPolicy completion_policy;
            bool completion_published = false;
            std::thread wait_thread;
        };

        struct WaitOutcome {
            pid_t waited = -1;
            int status = 0;
            int wait_error = 0;
            background_process_terminal_status terminal_status = background_process_terminal_status::unknown;
            std::optional<int> exit_code = -1;
            std::optional<int> signal_number;
        };

        mutable std::mutex mutex;
        std::condition_variable shutdown_cv;
        std::vector<std::shared_ptr<ProcessEntry>> entries;
        std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> entries_by_id;
        base::u64 next_id = 0;
        std::filesystem::path temp_root = std::filesystem::temp_directory_path() / ("orangutan-processes-" + make_process_token());
        BackgroundProcessManager::CompletionCallback completion_callback;
        bool shutting_down = false;
        std::size_t pending_starts = 0;

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;

        explicit Impl(BackgroundProcessManager::CompletionCallback callback)
        : completion_callback(std::move(callback)) {
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

        void begin_start(ProcessEntry &entry) {
            std::scoped_lock lock(mutex);
            if (shutting_down) {
                throw std::runtime_error("background process manager is shutting down");
            }
            ++pending_starts;
            entry.process_id = "proc-" + std::to_string(++next_id);
        }

        void finish_start() {
            std::scoped_lock lock(mutex);
            if (pending_starts > 0) {
                --pending_starts;
            }
            shutdown_cv.notify_all();
        }

        void register_entry(const std::shared_ptr<ProcessEntry> &entry) {
            std::scoped_lock lock(mutex);
            if (shutting_down) {
                throw std::runtime_error("background process manager is shutting down");
            }
            entries.push_back(entry);
            try {
                entries_by_id.insert_or_assign(entry->process_id, entry);
            } catch (...) {
                entries.pop_back();
                throw;
            }
        }

        [[nodiscard]]
        static BackgroundProcessSummary summarize_entry(const std::shared_ptr<ProcessEntry> &entry) {
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

            summary.stdout_bytes = file_size_or_zero(entry->stdout_path);
            summary.stderr_bytes = file_size_or_zero(entry->stderr_path);
            return summary;
        }

        void erase_entry(const std::string &process_id) {
            std::scoped_lock lock(mutex);
            std::erase_if(entries, [&process_id](const auto &entry) {
                return entry->process_id == process_id;
            });
            entries_by_id.erase(process_id);
        }

        static void finalize_wait_thread(const std::shared_ptr<ProcessEntry> &entry) {
            std::thread wait_thread;
            {
                std::scoped_lock lock(entry->mutex);
                if (entry->running) {
                    return;
                }
                if (!entry->wait_thread.joinable()) {
                    return;
                }
                if (entry->wait_thread.get_id() == std::this_thread::get_id()) {
                    entry->wait_thread.detach();
                    return;
                }
                wait_thread = std::move(entry->wait_thread);
            }
            wait_thread.join();
        }

        [[nodiscard]]
        static WaitOutcome wait_for_process_exit(const std::shared_ptr<ProcessEntry> &entry) {
            WaitOutcome outcome;
            outcome.waited = waitpid(entry->pid, &outcome.status, 0);
            while (outcome.waited == -1 && errno == EINTR) {
                outcome.waited = waitpid(entry->pid, &outcome.status, 0);
            }
            outcome.wait_error = outcome.waited == -1 ? errno : 0;
            if (outcome.waited == -1) {
                spdlog::warn("waitpid() failed for background process {} (pid {}): {}", entry->process_id, entry->pid, std::strerror(outcome.wait_error));
                return outcome;
            }

            if (WIFEXITED(outcome.status)) {
                outcome.exit_code = WEXITSTATUS(outcome.status);
                outcome.terminal_status = background_process_terminal_status::exited;
            } else if (WIFSIGNALED(outcome.status)) {
                outcome.signal_number = WTERMSIG(outcome.status);
                outcome.exit_code = 128 + WTERMSIG(outcome.status);
                outcome.terminal_status = background_process_terminal_status::signaled;
            }

            return outcome;
        }

        [[nodiscard]]
        static std::optional<BackgroundProcessCompletionEvent> finalize_wait_outcome(const std::shared_ptr<ProcessEntry> &entry, WaitOutcome outcome,
                                                                                     bool has_completion_callback) {
            std::optional<BackgroundProcessCompletionEvent> completion_event;
            {
                std::scoped_lock entry_lock(entry->mutex);
                entry->running = false;
                entry->exit_code = outcome.exit_code;
                entry->signal_number = outcome.signal_number;

                if (entry->completion_policy.publish_completion_event && !entry->completion_published && has_completion_callback) {
                    entry->completion_published = true;

                    BackgroundProcessCompletionEvent event;
                    event.process_id = entry->process_id;
                    event.command = entry->command;
                    event.working_dir = entry->working_dir;
                    event.pid = static_cast<int>(entry->pid);
                    event.kill_requested = entry->kill_requested;
                    event.terminal_status = outcome.terminal_status;
                    event.exit_code = entry->exit_code;
                    event.signal_number = entry->signal_number;
                    event.metadata = entry->completion_policy.metadata;
                    completion_event = std::move(event);
                }
            }
            entry->cv.notify_all();

            if (!completion_event.has_value()) {
                return std::nullopt;
            }

            try {
                completion_event->stdout = to_output_metadata(read_output_tail(entry->stdout_path));
                completion_event->stderr = to_output_metadata(read_output_tail(entry->stderr_path));
            } catch (const std::exception &ex) {
                spdlog::error("background process completion event build failed for {}: {}", entry->process_id, ex.what());
                return std::nullopt;
            } catch (...) {
                spdlog::error("background process completion event build failed for {} with non-standard exception", entry->process_id);
                return std::nullopt;
            }

            return completion_event;
        }

        static void publish_completion_event(const std::shared_ptr<ProcessEntry> &entry, const BackgroundProcessManager::CompletionCallback &completion_callback,
                                             std::optional<BackgroundProcessCompletionEvent> completion_event) {
            if (!completion_event.has_value() || !completion_callback) {
                return;
            }

            try {
                completion_callback(*completion_event);
            } catch (const std::exception &ex) {
                spdlog::error("background process completion callback failed for {}: {}", entry->process_id, ex.what());
            } catch (...) {
                spdlog::error("background process completion callback failed for {} with non-standard exception", entry->process_id);
            }
        }

        [[nodiscard]]
        static BackgroundProcessSnapshot snapshot_entry(const std::shared_ptr<ProcessEntry> &entry) {
            auto snapshot = BackgroundProcessSnapshot{summarize_entry(entry)};
            auto stdout_capture = read_output_tail(entry->stdout_path);
            snapshot.stdout_output = std::move(stdout_capture.text);
            snapshot.stdout_truncated = stdout_capture.truncated;
            snapshot.stdout_bytes = stdout_capture.total_bytes;

            auto stderr_capture = read_output_tail(entry->stderr_path);
            snapshot.stderr_output = std::move(stderr_capture.text);
            snapshot.stderr_truncated = stderr_capture.truncated;
            snapshot.stderr_bytes = stderr_capture.total_bytes;
            return snapshot;
        }

        [[nodiscard]]
        static BackgroundProcessSnapshot terminate_entry(const std::shared_ptr<ProcessEntry> &entry) {
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
                auto snapshot = snapshot_entry(entry);
                finalize_wait_thread(entry);
                return snapshot;
            }

            signal_process(pid, SIGTERM);

            std::unique_lock lock(entry->mutex);
            if (!entry->cv.wait_for(lock, std::chrono::milliseconds(500), [&entry] {
                    return !entry->running;
                })) {
                lock.unlock();
                signal_process(pid, SIGKILL);
                lock.lock();
                static_cast<void>(entry->cv.wait_for(lock, std::chrono::seconds(2), [&entry] {
                    return !entry->running;
                }));
            }
            lock.unlock();
            auto snapshot = snapshot_entry(entry);
            finalize_wait_thread(entry);
            return snapshot;
        }

        void shutdown() {
            std::vector<std::shared_ptr<ProcessEntry>> active_entries;
            {
                std::unique_lock lock(mutex);
                shutting_down = true;
                shutdown_cv.wait(lock, [this] {
                    return pending_starts == 0;
                });
                active_entries = entries;
            }

            for (const auto &entry : active_entries) {
                static_cast<void>(terminate_entry(entry));
            }

            for (const auto &entry : active_entries) {
                finalize_wait_thread(entry);
            }

            std::error_code ec;
            std::filesystem::remove_all(temp_root, ec);
        }
    };

    BackgroundProcessManager::BackgroundProcessManager(CompletionCallback completion_callback)
    : impl_(std::make_unique<Impl>(std::move(completion_callback))) {}

    BackgroundProcessManager::~BackgroundProcessManager() = default;

    BackgroundProcessManager::BackgroundProcessManager(BackgroundProcessManager &&) noexcept = default;

    BackgroundProcessManager &BackgroundProcessManager::operator=(BackgroundProcessManager &&) noexcept = default;

    BackgroundProcessSummary BackgroundProcessManager::start(const SubprocessConfig &config, const std::string &display_command, BackgroundProcessCompletionPolicy completion) {
        struct PendingStartGuard {
            explicit PendingStartGuard(Impl &impl_ref)
            : impl(impl_ref) {}

            PendingStartGuard(const PendingStartGuard &) = delete;
            PendingStartGuard &operator=(const PendingStartGuard &) = delete;
            PendingStartGuard(PendingStartGuard &&) = delete;
            PendingStartGuard &operator=(PendingStartGuard &&) = delete;

            ~PendingStartGuard() {
                impl.finish_start();
            }

            Impl &impl;
        };

        auto entry = std::make_shared<Impl::ProcessEntry>();
        impl_->begin_start(*entry);
        PendingStartGuard pending_start(*impl_);

        entry->command = display_command.empty() ? config.command : display_command;
        entry->working_dir = config.working_dir;
        entry->stdout_path = impl_->temp_root / (entry->process_id + ".stdout");
        entry->stderr_path = impl_->temp_root / (entry->process_id + ".stderr");
        entry->completion_policy = std::move(completion);

        fileio::File stdout_file(entry->stdout_path, "wb");
        stdout_file.close();
        fileio::File stderr_file(entry->stderr_path, "wb");
        stderr_file.close();

        try {
            entry->pid = spawn_background_subprocess(config, entry->stdout_path, entry->stderr_path);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(entry->stdout_path, ec);
            std::filesystem::remove(entry->stderr_path, ec);
            throw;
        }

        try {
            impl_->register_entry(entry);
        } catch (...) {
            signal_process(entry->pid, SIGKILL);
            while (waitpid(entry->pid, nullptr, 0) == -1 && errno == EINTR) {
            }
            std::error_code ec;
            std::filesystem::remove(entry->stdout_path, ec);
            std::filesystem::remove(entry->stderr_path, ec);
            throw;
        }

        try {
            std::scoped_lock lock(entry->mutex);
            entry->wait_thread = std::thread([entry, completion_callback = impl_->completion_callback]() {
                auto pipeline = stdexec::just() | stdexec::then([entry] {
                                    return Impl::wait_for_process_exit(entry);
                                }) |
                                stdexec::then([entry, has_completion_callback = (completion_callback != nullptr)](const Impl::WaitOutcome &outcome) {
                                    return Impl::finalize_wait_outcome(entry, outcome, has_completion_callback);
                                }) |
                                stdexec::then([entry, completion_callback](std::optional<BackgroundProcessCompletionEvent> completion_event) {
                                    Impl::publish_completion_event(entry, completion_callback, std::move(completion_event));
                                });

                try {
                    static_cast<void>(execution::sync_wait_or_throw(std::move(pipeline), "background process completion pipeline"));
                } catch (const std::exception &ex) {
                    spdlog::error("background process completion pipeline failed for {}: {}", entry->process_id, ex.what());
                } catch (...) {
                    spdlog::error("background process completion pipeline failed for {} with non-standard exception", entry->process_id);
                }

                Impl::finalize_wait_thread(entry);
            });
        } catch (...) {
            impl_->erase_entry(entry->process_id);
            signal_process(entry->pid, SIGKILL);
            while (waitpid(entry->pid, nullptr, 0) == -1 && errno == EINTR) {
            }
            std::error_code ec;
            std::filesystem::remove(entry->stdout_path, ec);
            std::filesystem::remove(entry->stderr_path, ec);
            throw;
        }

        return Impl::summarize_entry(entry);
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
            summaries.push_back(Impl::summarize_entry(entry));
        }
        return summaries;
    }

    BackgroundProcessSnapshot BackgroundProcessManager::poll(const std::string &process_id) const {
        const auto entry = impl_->find_entry(process_id);
        if (entry == nullptr) {
            throw std::runtime_error("background process not found: " + process_id);
        }
        return Impl::snapshot_entry(entry);
    }

    BackgroundProcessSnapshot BackgroundProcessManager::kill(const std::string &process_id) {
        const auto entry = impl_->find_entry(process_id);
        if (entry == nullptr) {
            throw std::runtime_error("background process not found: " + process_id);
        }
        return Impl::terminate_entry(entry);
    }

} // namespace orangutan::process
