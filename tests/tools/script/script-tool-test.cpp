#include "tools/script/script-loader.hpp"
#include "test-helpers.hpp"
#include "utils/escape.hpp"

#include <concepts>
#include <filesystem>
#include <type_traits>
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    static_assert(std::same_as<decltype(&register_script_tools), void (*)(ToolRegistry &, const std::vector<ScriptToolConfig> &, const std::filesystem::path &,
                                                                          const ToolPermissionContext *, const ToolRuntimeContext *, const ApprovalCallback &)>);

    static_assert(std::same_as<decltype(&shell_escape), decltype(&utils::shell_single_quote_escape)>);

    TEST_CASE("shell_escape_simple_string") {
        CHECK(shell_escape("hello") == "'hello'");
    };

    TEST_CASE("shell_escape_with_single_quote") {
        CHECK(shell_escape("it's") == "'it'\\''s'");
    };

    TEST_CASE("shell_escape_with_special_chars") {
        const auto escaped = shell_escape("hello; rm -rf /");
        CHECK(escaped == "'hello; rm -rf /'");
    };

    TEST_CASE("shell_escape_empty_string") {
        CHECK(shell_escape("") == "''");
    };

    TEST_CASE("generate_schema_string") {
        const auto schema = generate_input_schema({{"url", "string"}});
        CHECK(schema["type"] == "object");
        CHECK(schema["properties"]["url"]["type"] == "string");
        CHECK(schema["required"].is_array());
    };

    TEST_CASE("generate_schema_multiple_params") {
        const auto schema = generate_input_schema({{"file", "string"}, {"line", "integer"}});
        CHECK(schema["properties"]["file"]["type"] == "string");
        CHECK(schema["properties"]["line"]["type"] == "integer");
        CHECK(schema["required"].size() == 2UL);
    };

    TEST_CASE("generate_schema_no_params") {
        const auto schema = generate_input_schema({});
        CHECK(schema["type"] == "object");
        CHECK(schema["properties"].empty());
        CHECK(schema["required"].empty());
    };

    TEST_CASE("generate_schema_unknown_type_defaults_to_string") {
        const auto schema = generate_input_schema({{"data", "array"}});
        CHECK(schema["properties"]["data"]["type"] == "string");
    };

    TEST_CASE("substitute_single_param") {
        nlohmann::json input = {{"url", "https://example.com"}};
        const auto result = substitute_params("curl -sL ${url}", input, {{"url", "string"}});
        CHECK(result.contains("example.com"));
        CHECK(result.contains('\''));
    };

    TEST_CASE("substitute_missing_param") {
        nlohmann::json input = nlohmann::json::object();
        const auto result = substitute_params("ls ${path}", input, {{"path", "string"}});
        CHECK(result == "ls ");
    };

    TEST_CASE("substitute_no_params") {
        nlohmann::json input = nlohmann::json::object();
        const auto result = substitute_params("docker ps", input, {});
        CHECK(result == "docker ps");
    };

    TEST_CASE("substitute_multiple_params") {
        nlohmann::json input = {{"pattern", "TODO"}, {"path", "src/"}};
        const auto result = substitute_params("grep ${pattern} ${path}", input, {{"pattern", "string"}, {"path", "string"}});
        CHECK(result.contains("TODO"));
        CHECK(result.contains("src/"));
    };

    TEST_CASE("script substitution preserves current shell quoting for apostrophes") {
        nlohmann::json input = {{"message", "it's time"}};

        const auto result = substitute_params("printf %s ${message}", input, {{"message", "string"}});

        CHECK(result == "printf %s 'it'\\''s time'");
    };

    TEST_CASE("register_user_script_tools_lets_config_choose_tool_names") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
                                                   .name = "ls",
                                                   .description = "User-selected ls",
                                                   .command = "exa -la ${path}",
                                                   .input_schema = {{"path", "string"}},
                                               },
                                               {
                                                   .name = "grep",
                                                   .description = "User-selected grep",
                                                   .command = "rg --color=never -n ${pattern} ${path}",
                                                   .input_schema = {{"pattern", "string"}, {"path", "string"}},
                                               }};
        register_script_tools(registry, tools);
        const auto defs = registry.definitions();
        CHECK(defs.size() == 2UL);

        bool has_ls = false;
        bool has_grep = false;
        for (const auto &def : defs) {
            if (def.name == "ls") {
                has_ls = true;
            }
            if (def.name == "grep") {
                has_grep = true;
            }
        }
        CHECK(has_ls);
        CHECK(has_grep);
    };

    TEST_CASE("skip_tool_missing_name") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "",
            .description = "No name",
            .command = "echo hi",
        }};
        register_script_tools(registry, tools);
        CHECK(registry.definitions().empty());
    };

    TEST_CASE("skip_tool_missing_command") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "bad-tool",
            .description = "No command",
            .command = "",
        }};
        register_script_tools(registry, tools);
        CHECK(registry.definitions().empty());
    };

    TEST_CASE("execute_simple_command") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "echo-hello",
            .description = "Echo hello",
            .command = "echo hello",
        }};
        register_script_tools(registry, tools);

        ToolUse call("id1", "echo-hello", nlohmann::json::object());
        const auto result = registry.execute(call);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.contains("hello"));
    };

    TEST_CASE("execute_with_param") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "echo-msg",
            .description = "Echo a message",
            .command = "echo ${msg}",
            .input_schema = {{"msg", "string"}},
        }};
        register_script_tools(registry, tools);

        ToolUse call("id1", "echo-msg", {{"msg", "world"}});
        const auto result = registry.execute(call);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.contains("world"));
    };

    TEST_CASE("execute_failing_command") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "fail-tool",
            .description = "Always fails",
            .command = "exit 42",
        }};
        register_script_tools(registry, tools);

        ToolUse call("id1", "fail-tool", nlohmann::json::object());
        const auto result = registry.execute(call);
        CHECK(result.is_error);
        CHECK(result.content.contains("42"));
    };

    TEST_CASE("execute_command_not_found") {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "missing-cmd",
            .description = "Missing command",
            .command = "nonexistent_command_xyz_12345",
        }};
        register_script_tools(registry, tools);

        ToolUse call("id1", "missing-cmd", nlohmann::json::object());
        const auto result = registry.execute(call);
        CHECK(result.is_error);
    };

    TEST_CASE("rejects_working_directory_outside_workspace") {
        const auto root = orangutan::testing::unique_test_root("script-tool-workspace");
        const auto workspace = root / "workspace";
        const auto outside = root / "outside";
        std::filesystem::create_directories(workspace);
        std::filesystem::create_directories(outside);

        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "pwd-tool",
            .description = "Print working directory",
            .command = "pwd",
            .working_dir = "../outside",
        }};
        register_script_tools(registry, tools, workspace);

        ToolUse call("id_ws_escape", "pwd-tool", nlohmann::json::object());
        const auto result = registry.execute(call);
        CHECK(result.is_error);
        CHECK(result.content.contains("workspace sandbox"));

        std::filesystem::remove_all(root);
    };

} // namespace
