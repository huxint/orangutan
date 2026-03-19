#include "features/tools/mcp/client.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <thread>
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

constexpr std::string_view protocol_version = "2024-11-05";
constexpr auto initialize_timeout = std::chrono::seconds(10);
constexpr auto graceful_shutdown_timeout = std::chrono::seconds(5);
constexpr auto terminate_timeout = std::chrono::seconds(2);

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1; // NOLINT(cppcoreguidelines-pro-type-vararg)
}

void close_if_open(int &fd) {
    if (fd == -1) {
        return;
    }
    close(fd);
    fd = -1;
}

bool wait_for_pid_exit(pid_t pid, std::chrono::steady_clock::time_point deadline) {
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return true;
        }
        if (result == -1 && errno == ECHILD) {
            return true;
        }
        if (result == -1 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void signal_process(pid_t pid, int signal_number) {
    if (pid <= 0) {
        return;
    }
    if (killpg(pid, signal_number) == -1 && errno != ESRCH) {
        (void)kill(pid, signal_number);
    }
}

std::string join_args(const Config::McpServerConfig &config) {
    std::string command = config.command;
    for (const auto &arg : config.args) {
        command += ' ';
        command += arg;
    }
    return command;
}

std::string flatten_json(const json &value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

std::string flatten_tool_result(const json &result) {
    if (const auto content_it = result.find("content"); content_it != result.end()) {
        if (content_it->is_string()) {
            return content_it->get<std::string>();
        }
        if (content_it->is_array()) {
            std::string flattened;
            for (const auto &item : *content_it) {
                std::string part;
                if (item.is_object()) {
                    if (item.contains("text")) {
                        part = flatten_json(item.at("text"));
                    } else if (item.contains("json")) {
                        part = flatten_json(item.at("json"));
                    } else {
                        part = item.dump();
                    }
                } else {
                    part = flatten_json(item);
                }
                if (part.empty()) {
                    continue;
                }
                if (!flattened.empty()) {
                    flattened += '\n';
                }
                flattened += part;
            }
            if (!flattened.empty()) {
                return flattened;
            }
        }
    }

    if (const auto structured_it = result.find("structuredContent"); structured_it != result.end()) {
        return flatten_json(*structured_it);
    }

    if (const auto text_it = result.find("text"); text_it != result.end()) {
        return flatten_json(*text_it);
    }

    return result.dump();
}

} // namespace

McpClient::McpClient(Config::McpServerConfig config)
: config_(std::move(config)) {}

McpClient::~McpClient() {
    disconnect();
}

void McpClient::connect() {
    if (is_connected()) {
        return;
    }

    spawn_process();

    try {
        const auto result = send_request("initialize",
                                         {
                                             {"protocolVersion", protocol_version},
                                             {"capabilities", json::object()},
                                             {"clientInfo", {{"name", "orangutan"}, {"version", "0.1.0"}}},
                                         },
                                         initialize_timeout);

        const auto server_protocol = result.value("protocolVersion", std::string{});
        if (!server_protocol.empty() && server_protocol != protocol_version) {
            spdlog::warn("MCP server '{}' reported protocol '{}', client requested '{}'", config_.name, server_protocol, protocol_version);
        }

        send_notification("notifications/initialized", json::object(), initialize_timeout);
    } catch (...) {
        disconnect();
        throw;
    }
}

void McpClient::disconnect() {
    close_if_open(stdin_fd_);

    if (child_pid_ != -1) {
        const auto graceful_deadline = std::chrono::steady_clock::now() + graceful_shutdown_timeout;
        if (!wait_for_pid_exit(child_pid_, graceful_deadline)) {
            signal_process(child_pid_, SIGTERM);
            const auto terminate_deadline = std::chrono::steady_clock::now() + terminate_timeout;
            if (!wait_for_pid_exit(child_pid_, terminate_deadline)) {
                signal_process(child_pid_, SIGKILL);
                (void)waitpid(child_pid_, nullptr, 0);
            }
        }
    }

    cleanup_fds();
    child_pid_ = -1;
    next_request_id_ = 1;
    read_buffer_.clear();
}

bool McpClient::is_connected() const {
    return child_pid_ != -1 && stdin_fd_ != -1 && stdout_fd_ != -1;
}

const Config::McpServerConfig &McpClient::config() const {
    return config_;
}

std::string McpClient::command_line() const {
    return join_args(config_);
}

std::vector<McpToolInfo> McpClient::list_tools() {
    if (!is_connected()) {
        throw std::runtime_error("MCP server '" + config_.name + "' is not connected");
    }

    const auto result = send_request("tools/list", json::object(), std::chrono::seconds(config_.timeout));
    std::vector<McpToolInfo> tools;

    const auto tool_array = result.find("tools");
    if (tool_array == result.end() || !tool_array->is_array()) {
        return tools;
    }

    for (const auto &item : *tool_array) {
        if (!item.is_object()) {
            continue;
        }

        McpToolInfo tool;
        tool.name = item.value("name", std::string{});
        tool.description = item.value("description", std::string{});
        if (const auto schema_it = item.find("inputSchema"); schema_it != item.end()) {
            tool.input_schema = *schema_it;
        } else if (const auto schema_it = item.find("input_schema"); schema_it != item.end()) {
            tool.input_schema = *schema_it;
        }

        if (!tool.name.empty()) {
            tools.push_back(std::move(tool));
        }
    }

    return tools;
}

std::string McpClient::call_tool(const std::string &tool_name, const json &arguments) {
    if (!is_connected()) {
        throw std::runtime_error("MCP server '" + config_.name + "' is not connected");
    }

    try {
        const auto result = send_request("tools/call", {{"name", tool_name}, {"arguments", arguments}}, std::chrono::seconds(config_.timeout));
        const auto flattened = flatten_tool_result(result);
        if (result.value("isError", false)) {
            throw std::runtime_error(flattened.empty() ? "MCP tool returned an error" : flattened);
        }
        return flattened;
    } catch (...) {
        disconnect();
        throw;
    }
}

void McpClient::spawn_process() {
    std::array<int, 2> stdin_pipe{};
    std::array<int, 2> stdout_pipe{};

    if (pipe(stdin_pipe.data()) == -1 || pipe(stdout_pipe.data()) == -1) {
        if (stdin_pipe[0] != 0 || stdin_pipe[1] != 0) {
            close_if_open(stdin_pipe[0]);
            close_if_open(stdin_pipe[1]);
        }
        if (stdout_pipe[0] != 0 || stdout_pipe[1] != 0) {
            close_if_open(stdout_pipe[0]);
            close_if_open(stdout_pipe[1]);
        }
        throw std::runtime_error("pipe() failed for MCP server '" + config_.name + "': " + std::string(std::strerror(errno)));
    }

    const pid_t pid = fork();
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error("fork() failed for MCP server '" + config_.name + "': " + std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        (void)setpgid(0, 0);

        close(stdin_pipe[1]);
        close(stdout_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        for (const auto &[key, value] : config_.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char *> argv;
        argv.reserve(config_.args.size() + 2);
        argv.push_back(config_.command.data());
        for (auto &arg : config_.args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        execvp(config_.command.c_str(), argv.data());
        write_child_error("execvp", config_.command);
        _exit(127);
    }

    child_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!set_nonblocking(stdin_fd_) || !set_nonblocking(stdout_fd_)) {
        disconnect();
        throw std::runtime_error("fcntl() failed for MCP server '" + config_.name + "'");
    }
}

void McpClient::cleanup_fds() {
    close_if_open(stdin_fd_);
    close_if_open(stdout_fd_);
}

void McpClient::write_message(const json &message, std::chrono::steady_clock::time_point deadline) {
    if (stdin_fd_ == -1) {
        throw std::runtime_error("stdin pipe is closed for MCP server '" + config_.name + "'");
    }

    std::string payload = message.dump();
    payload.push_back('\n');
    size_t written = 0;

    while (written < payload.size()) {
        const auto pending = std::string_view(payload).substr(written);
        const auto n = write(stdin_fd_, pending.data(), pending.size());
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fd{
                .fd = stdin_fd_,
                .events = POLLOUT,
                .revents = 0,
            };
            const auto timeout_ms = remaining_ms(deadline);
            if (timeout_ms <= 0 || poll(&fd, 1, timeout_ms) <= 0) {
                throw std::runtime_error("Timed out writing to MCP server '" + config_.name + "'");
            }
            continue;
        }

        throw std::runtime_error("Failed writing to MCP server '" + config_.name + "': " + std::string(std::strerror(errno)));
    }
}

json McpClient::read_message(std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (const auto newline = read_buffer_.find('\n'); newline != std::string::npos) {
            auto line = read_buffer_.substr(0, newline);
            read_buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            try {
                return json::parse(line);
            } catch (const std::exception &e) {
                throw std::runtime_error("Invalid JSON from MCP server '" + config_.name + "': " + std::string(e.what()));
            }
        }

        struct pollfd fd{
            .fd = stdout_fd_,
            .events = POLLIN,
            .revents = 0,
        };
        const auto timeout_ms = remaining_ms(deadline);
        if (timeout_ms <= 0) {
            throw std::runtime_error("Timed out waiting for response from MCP server '" + config_.name + "'");
        }

        const auto ready = poll(&fd, 1, timeout_ms);
        if (ready == 0) {
            throw std::runtime_error("Timed out waiting for response from MCP server '" + config_.name + "'");
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("poll() failed for MCP server '" + config_.name + "': " + std::string(std::strerror(errno)));
        }

        std::array<char, 4096> buffer{};
        const auto n = read(stdout_fd_, buffer.data(), buffer.size());
        if (n > 0) {
            read_buffer_.append(buffer.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            throw std::runtime_error("MCP server '" + config_.name + "' closed stdout");
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        throw std::runtime_error("read() failed for MCP server '" + config_.name + "': " + std::string(std::strerror(errno)));
    }
}

json McpClient::send_request(const std::string &method, const json &params, std::chrono::seconds timeout) {
    const int request_id = next_request_id_++;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    write_message(
        {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", method},
            {"params", params},
        },
        deadline);

    while (true) {
        auto response = read_message(deadline);
        if (!response.contains("id") || response.at("id") != request_id) {
            continue;
        }
        if (const auto error_it = response.find("error"); error_it != response.end()) {
            const auto message = error_it->value("message", std::string{"unknown MCP error"});
            throw std::runtime_error(message);
        }
        if (const auto result_it = response.find("result"); result_it != response.end()) {
            return *result_it;
        }
        throw std::runtime_error("Malformed response from MCP server '" + config_.name + "'");
    }
}

void McpClient::send_notification(const std::string &method, const json &params, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    write_message(
        {
            {"jsonrpc", "2.0"},
            {"method", method},
            {"params", params},
        },
        deadline);
}

} // namespace orangutan
