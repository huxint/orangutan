#include "features/tools/mcp/client.hpp"
#include "features/tools/mcp/manager.hpp"
#include "core/tools/tool.hpp"

#include <filesystem>
#include "support/ut.hpp"

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

boost::ut::suite mcp_client_suite = [] {
    using namespace boost::ut;

    "connects_and_lists_tools"_test = [] {
        McpClient client(make_server_config());
        client.connect();

        const auto tools = client.list_tools();
        expect(tools.size() == 2_ul);
        expect(tools[0].name == "echo");
        expect(tools[1].name == "structured");

        client.disconnect();
    };

    "calls_tool_and_flattens_text_content"_test = [] {
        McpClient client(make_server_config());
        client.connect();

        const auto result = client.call_tool("echo", {{"message", "hello from mcp"}});
        expect(result == "hello from mcp");

        client.disconnect();
    };

    "handshake_timeout_throws"_test = [] {
        McpClient client(make_server_config("handshake-timeout"));
        expect(throws<std::runtime_error>([&] {
            client.connect();
        }));
        expect(not client.is_connected());
    };

    "tool_timeout_disconnects_client"_test = [] {
        McpClient client(make_server_config("tool-timeout", 1));
        client.connect();

        expect(throws<std::runtime_error>([&] {
            static_cast<void>(client.call_tool("echo", {{"message", "slow"}}));
        }));
        expect(not client.is_connected());
    };

    "server_crash_during_call_disconnects_client"_test = [] {
        McpClient client(make_server_config("crash-on-call"));
        client.connect();

        expect(throws<std::runtime_error>([&] {
            static_cast<void>(client.call_tool("echo", {{"message", "boom"}}));
        }));
        expect(not client.is_connected());
    };

    "registers_prefixed_tools_into_registry"_test = [] {
        ToolRegistry registry;
        McpManager manager({make_server_config()});

        manager.connect_all();
        manager.register_tools(registry);

        const auto defs = registry.definitions();
        const auto it = std::ranges::find(defs, std::string("mock:echo"), &ToolDef::name);
        expect((it != defs.end()) >> fatal) << "expected prefixed mock:echo tool registration";
        expect(it->description == "Echo a message back");

        const auto result = registry.execute(ToolUseBlock{
            .id = "call-1",
            .name = "mock:echo",
            .input = {{"message", "via registry"}},
        });
        expect(not result.is_error);
        expect(result.content == "via registry");
    };
};

} // namespace
