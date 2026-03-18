#include "features/tools/core/internal.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace orangutan {
namespace {

struct ShellCommandResult {
    std::string output;
    int exit_code = 0;
};

ShellCommandResult run_command(const std::string &command, const std::string &working_dir = {}) {
    const auto write_all = [](int fd, std::string_view text) {
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
    };
    const auto write_child_error = [&write_all](std::string_view operation, std::string_view path) {
        std::string message(operation);
        message += '(';
        message += path;
        message += ") failed: ";
        message += std::strerror(errno);
        message += '\n';
        write_all(STDERR_FILENO, message);
    };

    std::array<int, 2> pipe_fd{};
    if (pipe(pipe_fd.data()) == -1) {
        throw std::runtime_error("pipe() failed");
    }

    const pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
            write_child_error("chdir", working_dir);
            _exit(127);
        }

        auto shell_command = command;
        auto shell_name = std::to_array("sh");
        auto shell_flag = std::to_array("-c");
        std::array<char *, 4> shell_argv = {
            shell_name.data(),
            shell_flag.data(),
            shell_command.data(),
            nullptr,
        };
        execv("/bin/sh", shell_argv.data());
        _exit(127);
    }

    close(pipe_fd[1]);

    std::string output;
    std::array<char, 4096> buffer{};
    ssize_t bytes = 0;
    while ((bytes = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<size_t>(bytes));
    }
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    return {
        .output = std::move(output),
        .exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
    };
}

std::string run_shell(const json &input, const std::string &workspace) {
    const auto command = input.at("command").get<std::string>();
    if (workspace.empty()) {
        spdlog::info("  [tool] shell: {}", command);
    } else {
        spdlog::info("  [tool] shell (cwd={}): {}", workspace, command);
    }

    auto [result, exit_code] = run_command(command, workspace);
    if (exit_code != 0) {
        result += "\n[exit code: " + std::to_string(exit_code) + "]";
    }

    constexpr size_t max_output = 8192;
    if (result.size() > max_output) {
        result = result.substr(0, max_output) + "\n... (truncated, total " + std::to_string(result.size()) + " bytes)";
    }

    return result;
}

} // namespace

void register_shell_tool(ToolRegistry &registry, const std::string &workspace) {
    registry.register_tool({.definition = {.name = "shell",
                                           .description = "Execute a shell command and return its output.",
                                           .input_schema = {{"type", "object"},
                                                            {"properties", {{"command", {{"type", "string"}, {"description", "The shell command to execute"}}}}},
                                                            {"required", json::array({"command"})}}},
                            .execute = [workspace](const json &input) {
                                return run_shell(input, workspace);
                            }});
}

} // namespace orangutan
