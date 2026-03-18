#include "features/tools/mcp/client.hpp"

#include "features/tools/mcp/manager.hpp"
#include "core/tools/tool.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

std::string mock_server_path() {
    auto path = std::filesystem::path(SOURCE_DIR) / "tests" / "fixtures" / "mcp" / "mock-server.sh";
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    return path.string();
}

Config::McpServerConfig make_server_config(const std::string &mode = "normal", int timeout = 2) {
    Config::McpServerConfig config;
    config.name = "mock";
    config.command = mock_server_path();
    config.timeout = timeout;
    if (!mode.empty()) {
        config.args.push_back(mode);
    }
    return config;
}

} // namespace

TEST(McpClientTest, ConnectsAndListsTools) {
    McpClient client(make_server_config());
    client.connect();

    const auto tools = client.list_tools();
    ASSERT_EQ(tools.size(), 2);
    EXPECT_EQ(tools[0].name, "echo");
    EXPECT_EQ(tools[1].name, "structured");

    client.disconnect();
}

TEST(McpClientTest, CallsToolAndFlattensTextContent) {
    McpClient client(make_server_config());
    client.connect();

    const auto result = client.call_tool("echo", {{"message", "hello from mcp"}});
    EXPECT_EQ(result, "hello from mcp");

    client.disconnect();
}

TEST(McpClientTest, HandshakeTimeoutThrows) {
    McpClient client(make_server_config("handshake-timeout"));
    EXPECT_THROW(client.connect(), std::runtime_error);
    EXPECT_FALSE(client.is_connected());
}

TEST(McpClientTest, ToolTimeoutDisconnectsClient) {
    McpClient client(make_server_config("tool-timeout", 1));
    client.connect();

    EXPECT_THROW(static_cast<void>(client.call_tool("echo", {{"message", "slow"}})), std::runtime_error);
    EXPECT_FALSE(client.is_connected());
}

TEST(McpClientTest, ServerCrashDuringCallDisconnectsClient) {
    McpClient client(make_server_config("crash-on-call"));
    client.connect();

    EXPECT_THROW(static_cast<void>(client.call_tool("echo", {{"message", "boom"}})), std::runtime_error);
    EXPECT_FALSE(client.is_connected());
}

TEST(McpManagerTest, RegistersPrefixedToolsIntoRegistry) {
    ToolRegistry registry;
    McpManager manager({make_server_config()});

    manager.connect_all();
    manager.register_tools(registry);

    const auto defs = registry.definitions();
    const auto it = std::ranges::find(defs, std::string("mock:echo"), &ToolDef::name);
    ASSERT_NE(it, defs.end());
    EXPECT_EQ(it->description, "Echo a message back");

    const auto result = registry.execute(ToolUseBlock{
        .id = "call-1",
        .name = "mock:echo",
        .input = {{"message", "via registry"}},
    });
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content, "via registry");
}
