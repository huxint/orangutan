#pragma once

#include "tools/mcp/mcp-client.hpp"
#include "tools/registry/tool.hpp"

#include <memory>
#include <vector>

namespace orangutan::tools {

    class McpManager {
    public:
        explicit McpManager(std::vector<Config::McpServerConfig> servers);
        ~McpManager();

        McpManager(const McpManager &) = delete;
        McpManager &operator=(const McpManager &) = delete;
        McpManager(McpManager &&) = delete;
        McpManager &operator=(McpManager &&) = delete;

        void connect_all();
        void register_tools(ToolRegistry &registry);
        void shutdown();

        [[nodiscard]]
        std::size_t connected_server_count() const;

        [[nodiscard]]
        std::size_t total_tool_count() const;

    private:
        struct ServerState {
            Config::McpServerConfig config;
            std::unique_ptr<McpClient> client;
            std::vector<McpToolInfo> tools;
            bool connected = false;
        };

        std::vector<ServerState> servers_;
    };

} // namespace orangutan::tools

namespace orangutan {

    using tools::McpManager;

} // namespace orangutan
