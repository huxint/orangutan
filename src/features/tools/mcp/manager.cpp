#include "features/tools/mcp/manager.hpp"

#include <algorithm>
#include <ranges>
#include <spdlog/spdlog.h>

namespace orangutan {

    McpManager::McpManager(std::vector<Config::McpServerConfig> servers) {
        servers_.reserve(servers.size());
        for (auto &server : servers) {
            servers_.push_back(ServerState{
                .config = std::move(server),
                .client = nullptr,
                .tools = {},
                .connected = false,
            });
        }
    }

    McpManager::~McpManager() {
        shutdown();
    }

    void McpManager::connect_all() {
        for (auto &server : servers_) {
            server.tools.clear();
            server.connected = false;
            server.client = std::make_unique<McpClient>(server.config);

            try {
                spdlog::info("Connecting MCP server '{}' with command '{}'", server.config.name, server.client->command_line());
                server.client->connect();
                server.tools = server.client->list_tools();
                server.connected = true;
                spdlog::info("Connected MCP server '{}' [{}]: {} tool(s)", server.config.name, server.client->command_line(), server.tools.size());
                if (server.tools.empty()) {
                    spdlog::warn("MCP server '{}' exposed no tools", server.config.name);
                }
            } catch (const std::exception &e) {
                spdlog::error("Failed to initialize MCP server '{}' [{}]: {}", server.config.name, server.client->command_line(), e.what());
                server.client->disconnect();
                server.connected = false;
                server.tools.clear();
            }
        }
    }

    void McpManager::register_tools(ToolRegistry &registry) {
        for (auto &server : servers_) {
            if (!server.connected || server.client == nullptr) {
                continue;
            }

            for (const auto &tool : server.tools) {
                const auto qualified_name = server.config.name + ":" + tool.name;
                registry.register_tool(Tool{
                    .definition =
                        {
                            .name = qualified_name,
                            .description = tool.description,
                            .input_schema = tool.input_schema,
                        },
                    .execute = [client = server.client.get(), server_name = server.config.name, tool_name = tool.name](const nlohmann::json &input) -> std::string {
                        spdlog::debug("  [mcp-tool] {}:{}", server_name, tool_name);
                        return client->call_tool(tool_name, input);
                    },
                });
            }
        }
    }

    void McpManager::shutdown() {
        for (auto &server : std::ranges::reverse_view(servers_)) {
            if (server.client != nullptr) {
                server.client->disconnect();
            }
            server.connected = false;
            server.tools.clear();
        }
    }

    size_t McpManager::connected_server_count() const {
        return static_cast<size_t>(std::count_if(servers_.begin(), servers_.end(), [](const ServerState &server) {
            return server.connected;
        }));
    }

    size_t McpManager::total_tool_count() const {
        size_t total = 0;
        for (const auto &server : servers_) {
            total += server.tools.size();
        }
        return total;
    }

} // namespace orangutan
