#include "features/tools/script/script-loader.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include "support/ut.hpp"

using namespace orangutan;

namespace {

boost::ut::suite script_tool_suite = [] {
    using namespace boost::ut;

    "shell_escape_simple_string"_test = [] {
        expect(shell_escape("hello") == "'hello'");
    };

    "shell_escape_with_single_quote"_test = [] {
        expect(shell_escape("it's") == "'it'\\''s'");
    };

    "shell_escape_with_special_chars"_test = [] {
        const auto escaped = shell_escape("hello; rm -rf /");
        expect(escaped == "'hello; rm -rf /'");
    };

    "shell_escape_empty_string"_test = [] {
        expect(shell_escape("") == "''");
    };

    "generate_schema_string"_test = [] {
        const auto schema = generate_input_schema({{"url", "string"}});
        expect(schema["type"] == "object");
        expect(schema["properties"]["url"]["type"] == "string");
        expect(schema["required"].is_array());
    };

    "generate_schema_multiple_params"_test = [] {
        const auto schema = generate_input_schema({{"file", "string"}, {"line", "integer"}});
        expect(schema["properties"]["file"]["type"] == "string");
        expect(schema["properties"]["line"]["type"] == "integer");
        expect(schema["required"].size() == 2_ul);
    };

    "generate_schema_no_params"_test = [] {
        const auto schema = generate_input_schema({});
        expect(schema["type"] == "object");
        expect(schema["properties"].empty());
        expect(schema["required"].empty());
    };

    "generate_schema_unknown_type_defaults_to_string"_test = [] {
        const auto schema = generate_input_schema({{"data", "array"}});
        expect(schema["properties"]["data"]["type"] == "string");
    };

    "substitute_single_param"_test = [] {
        json input = {{"url", "https://example.com"}};
        const auto result = substitute_params("curl -sL ${url}", input, {{"url", "string"}});
        expect(result.find("example.com") != std::string::npos);
        expect(result.find('\'') != std::string::npos);
    };

    "substitute_missing_param"_test = [] {
        json input = json::object();
        const auto result = substitute_params("ls ${path}", input, {{"path", "string"}});
        expect(result == "ls ");
    };

    "substitute_no_params"_test = [] {
        json input = json::object();
        const auto result = substitute_params("docker ps", input, {});
        expect(result == "docker ps");
    };

    "substitute_multiple_params"_test = [] {
        json input = {{"pattern", "TODO"}, {"path", "src/"}};
        const auto result = substitute_params("grep ${pattern} ${path}", input, {{"pattern", "string"}, {"path", "string"}});
        expect(result.find("TODO") != std::string::npos);
        expect(result.find("src/") != std::string::npos);
    };

    "register_user_script_tools_lets_config_choose_tool_names"_test = [] {
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
        expect(defs.size() == 2_ul);

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
        expect(has_ls);
        expect(has_grep);
    };

    "skip_tool_missing_name"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "",
            .description = "No name",
            .command = "echo hi",
        }};
        register_script_tools(registry, tools);
        expect(registry.definitions().empty());
    };

    "skip_tool_missing_command"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "bad-tool",
            .description = "No command",
            .command = "",
        }};
        register_script_tools(registry, tools);
        expect(registry.definitions().empty());
    };

    "execute_simple_command"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "echo-hello",
            .description = "Echo hello",
            .command = "echo hello",
        }};
        register_script_tools(registry, tools);

        ToolUseBlock call{
            .id = "id1",
            .name = "echo-hello",
            .input = json::object(),
        };
        const auto result = registry.execute(call);
        expect(not result.is_error);
        expect(result.content.find("hello") != std::string::npos);
    };

    "execute_with_param"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "echo-msg",
            .description = "Echo a message",
            .command = "echo ${msg}",
            .input_schema = {{"msg", "string"}},
        }};
        register_script_tools(registry, tools);

        ToolUseBlock call{
            .id = "id1",
            .name = "echo-msg",
            .input = {{"msg", "world"}},
        };
        const auto result = registry.execute(call);
        expect(not result.is_error);
        expect(result.content.find("world") != std::string::npos);
    };

    "execute_failing_command"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "fail-tool",
            .description = "Always fails",
            .command = "exit 42",
        }};
        register_script_tools(registry, tools);

        ToolUseBlock call{
            .id = "id1",
            .name = "fail-tool",
            .input = json::object(),
        };
        const auto result = registry.execute(call);
        expect(result.is_error);
        expect(result.content.find("42") != std::string::npos);
    };

    "execute_command_not_found"_test = [] {
        ToolRegistry registry;
        std::vector<ScriptToolConfig> tools = {{
            .name = "missing-cmd",
            .description = "Missing command",
            .command = "nonexistent_command_xyz_12345",
        }};
        register_script_tools(registry, tools);

        ToolUseBlock call{
            .id = "id1",
            .name = "missing-cmd",
            .input = json::object(),
        };
        const auto result = registry.execute(call);
        expect(result.is_error);
    };

    "rejects_working_directory_outside_workspace"_test = [] {
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
        register_script_tools(registry, tools, workspace.string());

        ToolUseBlock call{
            .id = "id_ws_escape",
            .name = "pwd-tool",
            .input = json::object(),
        };
        const auto result = registry.execute(call);
        expect(result.is_error);
        expect(result.content.find("workspace sandbox") != std::string::npos);

        std::filesystem::remove_all(root);
    };
};

} // namespace
