#pragma once

#include "infra/config/config.hpp"
#include "core/types.hpp"

#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

namespace orangutan {

struct McpToolInfo {
    std::string name;
    std::string description;
    json input_schema = json::object();
};

class McpClient {
public:
    explicit McpClient(Config::McpServerConfig config);
    ~McpClient();

    McpClient(const McpClient &) = delete;
    McpClient &operator=(const McpClient &) = delete;
    McpClient(McpClient &&) = delete;
    McpClient &operator=(McpClient &&) = delete;

    void connect();
    void disconnect();

    [[nodiscard]]
    bool is_connected() const;

    [[nodiscard]]
    const Config::McpServerConfig &config() const;

    [[nodiscard]]
    std::string command_line() const;

    [[nodiscard]]
    std::vector<McpToolInfo> list_tools();

    [[nodiscard]]
    std::string call_tool(const std::string &tool_name, const json &arguments);

private:
    Config::McpServerConfig config_;
    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int next_request_id_ = 1;
    std::string read_buffer_;

    void spawn_process();
    void cleanup_fds();
    void write_message(const json &message, std::chrono::steady_clock::time_point deadline);
    json read_message(std::chrono::steady_clock::time_point deadline);
    json send_request(const std::string &method, const json &params, std::chrono::seconds timeout);
    void send_notification(const std::string &method, const json &params, std::chrono::seconds timeout);
};

} // namespace orangutan
