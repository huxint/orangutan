#include "features/tools/script/script-loader.hpp"

#include <gtest/gtest.h>
#include <filesystem>

using namespace orangutan;

// ── shell_escape ────────────────────────────────

TEST(ScriptToolTest, ShellEscapeSimpleString) {
    EXPECT_EQ(shell_escape("hello"), "'hello'");
}

TEST(ScriptToolTest, ShellEscapeWithSingleQuote) {
    EXPECT_EQ(shell_escape("it's"), "'it'\\''s'");
}

TEST(ScriptToolTest, ShellEscapeWithSpecialChars) {
    auto escaped = shell_escape("hello; rm -rf /");
    EXPECT_EQ(escaped, "'hello; rm -rf /'");
}

TEST(ScriptToolTest, ShellEscapeEmptyString) {
    EXPECT_EQ(shell_escape(""), "''");
}

// ── generate_input_schema ───────────────────────

TEST(ScriptToolTest, GenerateSchemaString) {
    auto schema = generate_input_schema({{"url", "string"}});
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["properties"]["url"]["type"], "string");
    EXPECT_TRUE(schema["required"].is_array());
}

TEST(ScriptToolTest, GenerateSchemaMultipleParams) {
    auto schema = generate_input_schema({{"file", "string"}, {"line", "integer"}});
    EXPECT_EQ(schema["properties"]["file"]["type"], "string");
    EXPECT_EQ(schema["properties"]["line"]["type"], "integer");
    EXPECT_EQ(schema["required"].size(), 2);
}

TEST(ScriptToolTest, GenerateSchemaNoParams) {
    auto schema = generate_input_schema({});
    EXPECT_EQ(schema["type"], "object");
    EXPECT_TRUE(schema["properties"].empty());
    EXPECT_TRUE(schema["required"].empty());
}

TEST(ScriptToolTest, GenerateSchemaUnknownTypeDefaultsToString) {
    auto schema = generate_input_schema({{"data", "array"}});
    EXPECT_EQ(schema["properties"]["data"]["type"], "string");
}

// ── substitute_params ───────────────────────────

TEST(ScriptToolTest, SubstituteSingleParam) {
    json input = {{"url", "https://example.com"}};
    auto result = substitute_params("curl -sL ${url}", input, {{"url", "string"}});
    EXPECT_NE(result.find("example.com"), std::string::npos);
    // Should be shell-escaped
    EXPECT_NE(result.find('\''), std::string::npos);
}

TEST(ScriptToolTest, SubstituteMissingParam) {
    json input = json::object();
    auto result = substitute_params("ls ${path}", input, {{"path", "string"}});
    // ${path} replaced with empty string
    EXPECT_EQ(result, "ls ");
}

TEST(ScriptToolTest, SubstituteNoParams) {
    json input = json::object();
    auto result = substitute_params("docker ps", input, {});
    EXPECT_EQ(result, "docker ps");
}

TEST(ScriptToolTest, SubstituteMultipleParams) {
    json input = {{"pattern", "TODO"}, {"path", "src/"}};
    auto result = substitute_params("grep ${pattern} ${path}", input, {{"pattern", "string"}, {"path", "string"}});
    EXPECT_NE(result.find("TODO"), std::string::npos);
    EXPECT_NE(result.find("src/"), std::string::npos);
}

// ── Script tool registration ────────────────────

TEST(ScriptToolTest, RegisterUserScriptToolsLetsConfigChooseToolNames) {
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
    auto defs = registry.definitions();
    ASSERT_EQ(defs.size(), 2);

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
    EXPECT_TRUE(has_ls);
    EXPECT_TRUE(has_grep);
}

TEST(ScriptToolTest, SkipToolMissingName) {
    ToolRegistry registry;
    std::vector<ScriptToolConfig> tools = {{
        .name = "",
        .description = "No name",
        .command = "echo hi",
    }};
    register_script_tools(registry, tools);
    EXPECT_EQ(registry.definitions().size(), 0);
}

TEST(ScriptToolTest, SkipToolMissingCommand) {
    ToolRegistry registry;
    std::vector<ScriptToolConfig> tools = {{
        .name = "bad-tool",
        .description = "No command",
        .command = "",
    }};
    register_script_tools(registry, tools);
    EXPECT_EQ(registry.definitions().size(), 0);
}

// ── Script tool execution ───────────────────────

TEST(ScriptToolTest, ExecuteSimpleCommand) {
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
    auto result = registry.execute(call);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("hello"), std::string::npos);
}

TEST(ScriptToolTest, ExecuteWithParam) {
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
    auto result = registry.execute(call);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("world"), std::string::npos);
}

TEST(ScriptToolTest, ExecuteFailingCommand) {
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
    auto result = registry.execute(call);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("42"), std::string::npos);
}

TEST(ScriptToolTest, ExecuteCommandNotFound) {
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
    auto result = registry.execute(call);
    EXPECT_TRUE(result.is_error);
}

TEST(ScriptToolTest, RejectsWorkingDirectoryOutsideWorkspace) {
    const auto root = std::filesystem::temp_directory_path() / "orangutan_script_tool_workspace_test";
    const auto workspace = root / "workspace";
    const auto outside = root / "outside";
    std::filesystem::remove_all(root);
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
    auto result = registry.execute(call);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("workspace sandbox"), std::string::npos);

    std::filesystem::remove_all(root);
}
