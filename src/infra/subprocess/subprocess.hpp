#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

namespace orangutan {

struct SubprocessConfig {
    std::string command;
    std::string stdin_data;
    std::chrono::seconds timeout{30};
    std::string working_dir;
    bool use_shell = true;
};

struct SubprocessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out = false;
};

// Run a subprocess with poll-based I/O multiplexing, deadline timeout, and process group cleanup.
SubprocessResult run_subprocess(const SubprocessConfig &config);

[[nodiscard]]
inline auto run_subprocess_sender(SubprocessConfig config) {
    return stdexec::just(std::move(config)) | stdexec::then([](const SubprocessConfig &active_config) {
               return run_subprocess(active_config);
           });
}

struct BackgroundProcessSummary {
    std::string process_id;
    std::string command;
    std::string working_dir;
    int pid = -1;
    bool running = false;
    bool kill_requested = false;
    std::optional<int> exit_code;
    std::optional<int> signal_number;
    size_t stdout_bytes = 0;
    size_t stderr_bytes = 0;
};

struct BackgroundProcessSnapshot : BackgroundProcessSummary {
    std::string stdout_output;
    std::string stderr_output;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
};

enum class BackgroundProcessTerminalStatus {
    exited,
    signaled,
    unknown,
};

struct BackgroundProcessOutputMetadata {
    std::string tail;
    size_t total_bytes = 0;
    bool truncated = false;
};

struct BackgroundProcessCompletionPolicy {
    bool publish_completion_event = false;
    std::map<std::string, std::string> metadata;
};

struct BackgroundProcessCompletionEvent {
    std::string process_id;
    std::string command;
    std::string working_dir;
    int pid = -1;
    bool kill_requested = false;
    BackgroundProcessTerminalStatus terminal_status = BackgroundProcessTerminalStatus::unknown;
    std::optional<int> exit_code;
    std::optional<int> signal_number;
    BackgroundProcessOutputMetadata stdout;
    BackgroundProcessOutputMetadata stderr;
    std::map<std::string, std::string> metadata;
};

class BackgroundProcessManager {
public:
    using CompletionCallback = std::function<void(const BackgroundProcessCompletionEvent &)>;

    explicit BackgroundProcessManager(CompletionCallback completion_callback = {});
    ~BackgroundProcessManager();

    BackgroundProcessManager(const BackgroundProcessManager &) = delete;
    BackgroundProcessManager &operator=(const BackgroundProcessManager &) = delete;
    BackgroundProcessManager(BackgroundProcessManager &&) noexcept;
    BackgroundProcessManager &operator=(BackgroundProcessManager &&) noexcept;

    [[nodiscard]]
    BackgroundProcessSummary start(const SubprocessConfig &config, const std::string &display_command, BackgroundProcessCompletionPolicy completion = {});

    [[nodiscard]]
    std::vector<BackgroundProcessSummary> list() const;

    [[nodiscard]]
    BackgroundProcessSnapshot poll(const std::string &process_id) const;

    [[nodiscard]]
    BackgroundProcessSnapshot kill(const std::string &process_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orangutan
