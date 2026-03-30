#include "features/tools/mcp/client.hpp"
#include "features/tools/mcp/manager.hpp"
#include "core/tools/tool.hpp"

#include <filesystem>
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    std::string mock_server_path() {
        auto path = std::filesystem::path(SOURCE_DIR) / "tests" / "fixtures" / "mcp" / "mock-server.sh";
        std::filesystem::permissions(path, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
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

    TEST_CASE("connects_and_lists_tools") {
        McpClient client(make_server_config());
        client.connect();

        const auto tools = client.list_tools();
        CHECK(tools.size() == 2UL);
        CHECK(tools[0].name == "echo");
        CHECK(tools[1].name == "structured");

        client.disconnect();
    };

    TEST_CASE("calls_tool_and_flattens_text_content") {
        McpClient client(make_server_config());
        client.connect();

        const auto result = client.call_tool("echo", {{"message", "hello from mcp"}});
        CHECK(result == "hello from mcp");

        client.disconnect();
    };

    TEST_CASE("handshake_timeout_throws") {
        McpClient client(make_server_config("handshake-timeout"));
        REQUIRE_THROWS_AS(client.connect(), std::runtime_error);
        CHECK_FALSE(client.is_connected());
    };

    TEST_CASE("tool_timeout_disconnects_client") {
        McpClient client(make_server_config("tool-timeout", 1));
        client.connect();

        REQUIRE_THROWS_AS(client.call_tool("echo", {{"message", "slow"}}), std::runtime_error);
        CHECK_FALSE(client.is_connected());
    };

    TEST_CASE("server_crash_during_call_disconnects_client") {
        McpClient client(make_server_config("crash-on-call"));
        client.connect();

        REQUIRE_THROWS_AS(client.call_tool("echo", {{"message", "boom"}}), std::runtime_error);
        CHECK_FALSE(client.is_connected());
    };

    TEST_CASE("registers_prefixed_tools_into_registry") {
        ToolRegistry registry;
        McpManager manager({make_server_config()});

        manager.connect_all();
        manager.register_tools(registry);

        const auto defs = registry.definitions();
        const auto it = std::ranges::find(defs, std::string("mock:echo"), &ToolDef::name);
        INFO("expected prefixed mock:echo tool registration");
        REQUIRE(it != defs.end());
        CHECK(it->description == "Echo a message back");

        const auto result = registry.execute(ToolUse("call-1", "mock:echo", {{"message", "via registry"}}));
        CHECK_FALSE(result.is_error);
        CHECK(result.content == "via registry");
    };

} // namespace
