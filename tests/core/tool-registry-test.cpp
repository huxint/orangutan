#include "infra/config/config.hpp"
#include "features/tools/runtime/runtime-loader.hpp"
#include "core/tools/tool.hpp"
#include "features/tools/script/script-loader.hpp"
#include "features/tools/core/hashline.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "infra/storage/session-store.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "test-helpers.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace orangutan;
using orangutan::testing::test_tmp_root;

namespace {

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&](const ToolDef &definition) {
        return definition.name == name;
    });
}

json start_background_process(ToolRegistry &registry, const std::string &command, const std::string &working_dir = {}) {
    json input = {
        {"command", command},
        {"background", true},
    };
    if (!working_dir.empty()) {
        input["working_dir"] = working_dir;
    }

    const auto result = registry.execute(ToolUseBlock{
        .id = "background-shell",
        .name = "shell",
        .input = std::move(input),
    });
    EXPECT_FALSE(result.is_error);
    if (result.is_error) {
        return {};
    }

    return json::parse(result.content);
}

json wait_for_background_process(ToolRegistry &registry, const std::string &process_id, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    json last_snapshot;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = registry.execute(ToolUseBlock{
            .id = "poll-background",
            .name = "process_poll",
            .input = {{"process_id", process_id}},
        });
        EXPECT_FALSE(result.is_error);
        if (result.is_error) {
            return {};
        }

        last_snapshot = json::parse(result.content);
        if (!last_snapshot.value("running", true)) {
            return last_snapshot;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ADD_FAILURE() << "background process did not finish in time: " << process_id;
    return last_snapshot;
}

ToolRuntimeContext make_runtime_tool_context(SubagentManager *manager, std::string *current_session_id = nullptr, std::vector<std::string> allowed_child_agents = {"reviewer"}) {
    return ToolRuntimeContext{
        .runtime_key = "runtime:cli:default",
        .agent_key = "default",
        .scope_key = "scope:parent",
        .current_session_id = current_session_id,
        .allowed_child_agents = std::move(allowed_child_agents),
        .subagent_manager = manager,
        .runtime_origin = SubagentRuntimeOrigin::cli,
        .raw_caller_id = "cli:local",
    };
}

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

} // namespace

// ── ToolRegistry basics ─────────────────────────

TEST(ToolRegistryTest, StartsEmpty) {
    const ToolRegistry registry;
    EXPECT_TRUE(registry.definitions().empty());
}

TEST(ToolRegistryTest, RegisterAndRetrieveDefinition) {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "echo", .description = "Echoes input", .input_schema = {{"type", "object"}}}, .execute = [](const json &input) {
                                return input.at("text").get<std::string>();
                            }});

    const auto defs = registry.definitions();
    ASSERT_EQ(defs.size(), 1);
    EXPECT_EQ(defs[0].name, "echo");
    EXPECT_EQ(defs[0].description, "Echoes input");
}

TEST(ToolRegistryTest, ExecutesRegisteredTool) {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "greet", .description = "Greets"}, .execute = [](const json &input) {
                                return "Hello, " + input.at("name").get<std::string>() + "!";
                            }});

    const ToolUseBlock call{
        .id = "id_1",
        .name = "greet",
        .input = {{"name", "Alice"}},
    };
    const auto result = registry.execute(call);

    EXPECT_EQ(result.tool_use_id, "id_1");
    EXPECT_EQ(result.content, "Hello, Alice!");
    EXPECT_FALSE(result.is_error);
}

TEST(ToolRegistryTest, UnknownToolReturnsError) {
    const ToolRegistry registry;
    const ToolUseBlock call{
        .id = "id_2",
        .name = "nonexistent",
        .input = {},
    };
    const auto result = registry.execute(call);

    EXPECT_EQ(result.tool_use_id, "id_2");
    EXPECT_TRUE(result.is_error);
}

TEST(ToolRegistryTest, ToolExceptionBecomesError) {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "boom", .description = "Always fails"}, .execute = [](const json &) -> std::string {
                                throw std::runtime_error("kaboom");
                            }});

    const ToolUseBlock call{
        .id = "id_3",
        .name = "boom",
        .input = {},
    };
    const auto result = registry.execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("kaboom"), std::string::npos);
}

TEST(ToolRegistryTest, DuplicateRegistrationReplacesExistingExecutor) {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "echo", .description = "First"}, .execute = [](const json &) {
                                return std::string{"first"};
                            }});
    registry.register_tool({.definition = {.name = "echo", .description = "Second"}, .execute = [](const json &) {
                                return std::string{"second"};
                            }});

    const auto defs = registry.definitions();
    ASSERT_EQ(defs.size(), 1);
    EXPECT_EQ(defs[0].description, "Second");

    const auto result = registry.execute(ToolUseBlock{
        .id = "id_dup",
        .name = "echo",
        .input = {},
    });
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content, "second");
}

// ── Built-in tools ──────────────────────────────

class BuiltinToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        register_builtin_tools(registry_);
    }

    [[nodiscard]]
    ToolRegistry &registry() {
        return registry_;
    }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    ToolRegistry registry_;
};

class BuiltinToolsWorkspaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        workspace_ = test_tmp_root() / "orangutan_tool_workspace_test";
        std::filesystem::remove_all(workspace_);
        std::filesystem::create_directories(workspace_);
        register_builtin_tools(registry_, nullptr, workspace_.string());
    }

    void TearDown() override {
        tmp_env_.reset();
        std::filesystem::remove_all(workspace_);
    }

    [[nodiscard]]
    const std::filesystem::path &workspace() const {
        return workspace_;
    }

    [[nodiscard]]
    ToolRegistry &registry() {
        return registry_;
    }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path workspace_;
    ToolRegistry registry_;
};

class BuiltinToolsWorkspaceConfigAccessTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        temp_root_ = test_tmp_root() / "orangutan_tool_config_access_test";
        home_ = temp_root_ / "home";
        workspace_ = temp_root_ / "workspace";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(home_ / ".orangutan");
        std::filesystem::create_directories(workspace_);
        home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
        register_builtin_tools(registry_, nullptr, workspace_.string());
    }

    void TearDown() override {
        home_env_.reset();
        tmp_env_.reset();
        std::filesystem::remove_all(temp_root_);
    }

    [[nodiscard]]
    const std::filesystem::path &home() const {
        return home_;
    }

    [[nodiscard]]
    ToolRegistry &registry() {
        return registry_;
    }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path temp_root_;
    std::filesystem::path home_;
    std::filesystem::path workspace_;
    std::unique_ptr<ScopedEnvVar> home_env_;
    ToolRegistry registry_;
};

TEST_F(BuiltinToolsTest, RegistersExpectedCoreTools) {
    const auto defs = registry().definitions();
    EXPECT_EQ(defs.size(), 7);
    EXPECT_TRUE(has_tool_named(defs, "shell"));
    EXPECT_TRUE(has_tool_named(defs, "process_list"));
    EXPECT_TRUE(has_tool_named(defs, "process_poll"));
    EXPECT_TRUE(has_tool_named(defs, "process_kill"));
    EXPECT_TRUE(has_tool_named(defs, "read"));
    EXPECT_TRUE(has_tool_named(defs, "write"));
    EXPECT_TRUE(has_tool_named(defs, "edit"));
}

TEST_F(BuiltinToolsTest, DoesNotRegisterSubagentToolsWithoutRuntimeContext) {
    const auto defs = registry().definitions();

    for (const auto &def : defs) {
        EXPECT_NE(def.name, "subagent_spawn");
        EXPECT_NE(def.name, "subagent_status");
        EXPECT_NE(def.name, "subagent_wait");
    }
}

TEST_F(BuiltinToolsTest, DoesNotRegisterMemoryToolsWithoutMemoryStore) {
    const auto defs = registry().definitions();

    EXPECT_FALSE(has_tool_named(defs, "remember"));
    EXPECT_FALSE(has_tool_named(defs, "recall"));
    EXPECT_FALSE(has_tool_named(defs, "forget"));
    EXPECT_FALSE(has_tool_named(defs, "memory_store"));
    EXPECT_FALSE(has_tool_named(defs, "memory_recall"));
    EXPECT_FALSE(has_tool_named(defs, "memory_forget"));
    EXPECT_FALSE(has_tool_named(defs, "memory_update"));
    EXPECT_FALSE(has_tool_named(defs, "memory_list"));
    EXPECT_FALSE(has_tool_named(defs, "memory_stats"));
}

TEST_F(BuiltinToolsTest, ShellRunsSimpleCommand) {
    const ToolUseBlock call{
        .id = "id_sh",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("hello"), std::string::npos);
}

TEST_F(BuiltinToolsTest, ShellReportsNonZeroExitCode) {
    const ToolUseBlock call{
        .id = "id_sh2",
        .name = "shell",
        .input = {{"command", "false"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("exit code"), std::string::npos);
}

TEST_F(BuiltinToolsTest, ReadFileReturnsLineNumberedContents) {
    const auto tmp = test_tmp_root() / "orangutan_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "aaa\nbbb\nccc\n";
    }

    const ToolUseBlock call{
        .id = "id_rf",
        .name = "read",
        .input = {{"path", tmp.string()}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    // cat -n format: right-aligned 6-char field, tab, content
    EXPECT_NE(result.content.find("1\taaa\n"), std::string::npos);
    EXPECT_NE(result.content.find("2\tbbb\n"), std::string::npos);
    EXPECT_NE(result.content.find("3\tccc\n"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadFileMissingReturnsError) {
    const ToolUseBlock call{
        .id = "id_rf2",
        .name = "read",
        .input = {{"path", "/tmp/orangutan_nonexistent_file_xyz.txt"}},
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("not found"), std::string::npos);
}

// ── write tool ──────────────────────────────────

TEST_F(BuiltinToolsTest, WriteCreatesFile) {
    const auto tmp = test_tmp_root() / "orangutan_write_test.txt";
    std::filesystem::remove(tmp); // ensure clean state

    const ToolUseBlock call{
        .id = "id_wf",
        .name = "write",
        .input = {{"path", tmp.string()}, {"content", "hello world"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("11 bytes"), std::string::npos);

    // Verify file contents
    std::ifstream ifs(tmp);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello world");

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, WriteCreatesParentDirectories) {
    const auto tmp = test_tmp_root() / "orangutan_test_dir" / "sub" / "file.txt";
    std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");

    const ToolUseBlock call{
        .id = "id_wf2",
        .name = "write",
        .input = {{"path", tmp.string()}, {"content", "nested"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_TRUE(std::filesystem::exists(tmp));

    std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");
}

// ── read tool — offset/limit ────────────────────

TEST_F(BuiltinToolsTest, ReadFileWithOffset) {
    const auto tmp = test_tmp_root() / "orangutan_offset_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 20; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUseBlock call{
        .id = "id_off",
        .name = "read",
        .input = {{"path", tmp.string()}, {"offset", 10}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("10\tline10"), std::string::npos);
    EXPECT_NE(result.content.find("20\tline20"), std::string::npos);
    // Should NOT contain lines before offset
    EXPECT_EQ(result.content.find("\tline9\n"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadFileWithLimit) {
    const auto tmp = test_tmp_root() / "orangutan_limit_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 100; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUseBlock call{
        .id = "id_lim",
        .name = "read",
        .input = {{"path", tmp.string()}, {"limit", 5}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("1\tline1"), std::string::npos);
    EXPECT_NE(result.content.find("5\tline5"), std::string::npos);
    // Should have truncation summary
    EXPECT_NE(result.content.find("showing 5 of 100 lines"), std::string::npos);
    // Should NOT contain line 6
    EXPECT_EQ(result.content.find("\tline6\n"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadFileWithOffsetAndLimit) {
    const auto tmp = test_tmp_root() / "orangutan_offlim_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 500; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUseBlock call{
        .id = "id_ol",
        .name = "read",
        .input = {{"path", tmp.string()}, {"offset", 100}, {"limit", 50}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("100\tline100"), std::string::npos);
    EXPECT_NE(result.content.find("149\tline149"), std::string::npos);
    EXPECT_NE(result.content.find("showing 50 of 500 lines"), std::string::npos);
    EXPECT_EQ(result.content.find("\tline99\n"), std::string::npos);
    EXPECT_EQ(result.content.find("\tline150\n"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadFileOffsetBeyondEOF) {
    const auto tmp = test_tmp_root() / "orangutan_eof_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "only\nthree\nlines\n";
    }

    const ToolUseBlock call{
        .id = "id_eof",
        .name = "read",
        .input = {{"path", tmp.string()}, {"offset", 100}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("No content at offset 100"), std::string::npos);
    EXPECT_NE(result.content.find("file has 3 lines"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadRejectsNonPositiveOffset) {
    const auto tmp = test_tmp_root() / "orangutan_invalid_offset_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "line1\n";
    }

    const ToolUseBlock call{
        .id = "id_invalid_offset",
        .name = "read",
        .input = {{"path", tmp.string()}, {"offset", 0}},
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("offset"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadRejectsNonPositiveLimit) {
    const auto tmp = test_tmp_root() / "orangutan_invalid_limit_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "line1\n";
    }

    const ToolUseBlock call{
        .id = "id_invalid_limit",
        .name = "read",
        .input = {{"path", tmp.string()}, {"limit", 0}},
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("limit"), std::string::npos);

    std::filesystem::remove(tmp);
}

// ── read tool — binary detection ────────────────

TEST_F(BuiltinToolsTest, ReadBinaryFileReturnsMetadata) {
    const auto tmp = test_tmp_root() / "orangutan_binary_test.bin";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        ofs << "header";
        ofs.put('\0');
        ofs << "binary data here";
    }

    const ToolUseBlock call{
        .id = "id_bin",
        .name = "read",
        .input = {{"path", tmp.string()}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("Binary file:"), std::string::npos);
    EXPECT_NE(result.content.find(".bin"), std::string::npos);
    EXPECT_NE(result.content.find("bytes"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadTextFileReadsNormally) {
    const auto tmp = test_tmp_root() / "orangutan_text_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "just text\nno nulls\n";
    }

    const ToolUseBlock call{
        .id = "id_txt",
        .name = "read",
        .input = {{"path", tmp.string()}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content.find("Binary file:"), std::string::npos);
    EXPECT_NE(result.content.find("just text"), std::string::npos);

    std::filesystem::remove(tmp);
}

// ── read tool — multi-path ──────────────────────

TEST_F(BuiltinToolsTest, ReadMultipleFiles) {
    const auto tmp_a = test_tmp_root() / "orangutan_multi_a.txt";
    const auto tmp_b = test_tmp_root() / "orangutan_multi_b.txt";
    {
        std::ofstream(tmp_a) << "content_a\n";
        std::ofstream(tmp_b) << "content_b\n";
    }

    const ToolUseBlock call{
        .id = "id_multi",
        .name = "read",
        .input = {{"paths", json::array({tmp_a.string(), tmp_b.string()})}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("=== " + tmp_a.string() + " ==="), std::string::npos);
    EXPECT_NE(result.content.find("=== " + tmp_b.string() + " ==="), std::string::npos);
    EXPECT_NE(result.content.find("content_a"), std::string::npos);
    EXPECT_NE(result.content.find("content_b"), std::string::npos);

    std::filesystem::remove(tmp_a);
    std::filesystem::remove(tmp_b);
}

TEST_F(BuiltinToolsTest, ReadMultiPathOneFailsContinuesOthers) {
    const auto tmp = test_tmp_root() / "orangutan_multi_ok.txt";
    {
        std::ofstream(tmp) << "good content\n";
    }

    const ToolUseBlock call{
        .id = "id_mpf",
        .name = "read",
        .input = {{"paths", json::array({tmp.string(), "/tmp/orangutan_nonexistent_xyz.txt"})}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("good content"), std::string::npos);
    EXPECT_NE(result.content.find("Error:"), std::string::npos);

    std::filesystem::remove(tmp);
}

TEST_F(BuiltinToolsTest, ReadBothPathAndPathsReturnsError) {
    const ToolUseBlock call{
        .id = "id_both",
        .name = "read",
        .input = {{"path", "/tmp/a.txt"}, {"paths", json::array({"/tmp/b.txt"})}},
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("not both"), std::string::npos);
}

TEST_F(BuiltinToolsTest, ReadNeitherPathNorPathsReturnsError) {
    const ToolUseBlock call{
        .id = "id_neither",
        .name = "read",
        .input = json::object(),
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("Required"), std::string::npos);
}

// ── user-selected script tools ───────────────────

class ScriptToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        register_builtin_tools(registry_);
        register_script_tools(registry_, {{
                                              .name = "ls",
                                              .description = "List files",
                                              .command = "ls -la ${path}",
                                              .input_schema = {{"path", "string"}},
                                          },
                                          {
                                              .name = "grep",
                                              .description = "Search files",
                                              .command = "rg --color=never -n ${pattern} ${path}",
                                              .input_schema = {{"pattern", "string"}, {"path", "string"}},
                                          }});
    }

    [[nodiscard]]
    ToolRegistry &registry() {
        return registry_;
    }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    ToolRegistry registry_;
};

TEST(RuntimeToolLoaderTest, RegistersBuiltinAndCustomToolsOnly) {
    ToolRegistry registry;
    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "printf custom",
    }};

    const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {});
    const auto defs = registry.definitions();

    EXPECT_EQ(result.mcp_tool_count, 0);
    EXPECT_EQ(result.mcp_manager, nullptr);
    EXPECT_TRUE(has_tool_named(defs, "shell"));
    EXPECT_TRUE(has_tool_named(defs, "process_list"));
    EXPECT_TRUE(has_tool_named(defs, "process_poll"));
    EXPECT_TRUE(has_tool_named(defs, "process_kill"));
    EXPECT_TRUE(has_tool_named(defs, "read"));
    EXPECT_TRUE(has_tool_named(defs, "write"));
    EXPECT_TRUE(has_tool_named(defs, "edit"));
    EXPECT_FALSE(has_tool_named(defs, "ls"));
    EXPECT_FALSE(has_tool_named(defs, "grep"));
    EXPECT_TRUE(has_tool_named(defs, "echo_custom"));
}

TEST(RuntimeToolLoaderTest, RegistersUsableMemoryAndSubagentToolsTogether) {
    ToolRegistry registry;
    // Real child execution is covered in SubagentIntegrationTest; this regression test only checks that runtime bootstrap wires usable memory and subagent control tools together
    // in one registry.
    const auto memory_db = test_tmp_root() / "orangutan_runtime_tool_loader_memory.db";
    const auto session_db = test_tmp_root() / "orangutan_runtime_tool_loader_sessions.db";
    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);

    {
        MemoryStore memory_store(memory_db.string());
        RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{.scope = "scope:parent"});
        SessionStore session_store(session_db.string());
        const auto parent_session_id = session_store.create_empty("test-model", "scope:parent");
        const auto child_session_id = session_store.create_empty("test-model", "scope:child");

        {
            SubagentRunStore run_store(session_db.string());
            SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
                return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
            });
            auto current_session_id = parent_session_id;
            const auto tool_context = make_runtime_tool_context(&manager, &current_session_id);

            const auto result = register_runtime_tools(registry, &runtime_memory, {}, &tool_context, {}, {});
            const auto defs = registry.definitions();

            EXPECT_EQ(result.mcp_tool_count, 0);
            EXPECT_EQ(result.mcp_manager, nullptr);
            EXPECT_TRUE(has_tool_named(defs, "shell"));
            EXPECT_TRUE(has_tool_named(defs, "process_list"));
            EXPECT_TRUE(has_tool_named(defs, "process_poll"));
            EXPECT_TRUE(has_tool_named(defs, "process_kill"));
            EXPECT_TRUE(has_tool_named(defs, "read"));
            EXPECT_FALSE(has_tool_named(defs, "ls"));
            EXPECT_FALSE(has_tool_named(defs, "grep"));
            EXPECT_TRUE(has_tool_named(defs, "subagent_spawn"));
            EXPECT_TRUE(has_tool_named(defs, "subagent_status"));
            EXPECT_TRUE(has_tool_named(defs, "subagent_wait"));
            EXPECT_TRUE(has_tool_named(defs, "remember"));
            EXPECT_TRUE(has_tool_named(defs, "memory_recall"));
            EXPECT_TRUE(has_tool_named(defs, "memory_stats"));

            const auto remember = registry.execute(ToolUseBlock{
                .id = "remember-runtime",
                .name = "remember",
                .input = {{"key", "theme"}, {"content", "blue"}, {"category", "prefs"}},
            });
            EXPECT_FALSE(remember.is_error);

            const auto recall = registry.execute(ToolUseBlock{
                .id = "recall-runtime",
                .name = "memory_recall",
                .input = {{"query", "theme"}},
            });
            EXPECT_FALSE(recall.is_error);
            EXPECT_NE(recall.content.find("blue"), std::string::npos);

            const auto spawn = registry.execute(ToolUseBlock{
                .id = "spawn-runtime",
                .name = "subagent_spawn",
                .input = {{"child_agent_key", "reviewer"},
                          {"child_scope_key", "scope:child"},
                          {"child_session_id", child_session_id},
                          {"task_summary", "Review the tool layout refactor"}},
            });
            EXPECT_FALSE(spawn.is_error);
            const auto spawn_payload = json::parse(spawn.content);
            ASSERT_TRUE(spawn_payload.at("accepted").get<bool>());
            const auto run_id = spawn_payload.at("run_id").get<std::string>();
            EXPECT_FALSE(run_id.empty());

            const auto wait = registry.execute(ToolUseBlock{
                .id = "wait-runtime",
                .name = "subagent_wait",
                .input = {{"run_id", run_id}, {"timeout_ms", 1000}},
            });
            EXPECT_FALSE(wait.is_error);
            const auto wait_payload = json::parse(wait.content);
            EXPECT_EQ(wait_payload.at("state").get<std::string>(), "completed");
        }
    }

    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);
}

TEST(RuntimeToolLoaderTest, DeniedToolsAreHiddenAndBlockedByPolicy) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.denied_tools = {"shell"};

    const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);
    const auto defs = registry.definitions();

    EXPECT_EQ(result.mcp_tool_count, 0);
    EXPECT_EQ(result.mcp_manager, nullptr);
    EXPECT_FALSE(has_tool_named(defs, "shell"));
    EXPECT_TRUE(has_tool_named(defs, "read"));

    const auto shell_result = registry.execute(ToolUseBlock{
        .id = "deny-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(shell_result.is_error);
    EXPECT_NE(shell_result.content.find("permission policy"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, ShellApprovalAskBlocksWhenPromptUnavailable) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;

    (void)register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);

    const auto shell_result = registry.execute(ToolUseBlock{
        .id = "ask-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(shell_result.is_error);
    EXPECT_NE(shell_result.content.find("requires approval"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, ShellApprovalCallbackCanAllowCommand) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;

    bool prompted = false;
    (void)register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions, [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
        prompted = true;
        EXPECT_EQ(call.name, "shell");
        EXPECT_NE(prompt_text.find("echo hello"), std::string::npos);
        return true;
    });

    const auto shell_result = registry.execute(ToolUseBlock{
        .id = "allow-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(prompted);
    EXPECT_FALSE(shell_result.is_error);
    EXPECT_NE(shell_result.content.find("hello"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, UsesDynamicApprovalCallbackFromToolContext) {
    SubagentRunStore run_store(":memory:");
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;
    auto tool_context = make_runtime_tool_context(&manager);
    bool prompted = false;

    (void)register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions);
    tool_context.approval_callback = [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
        prompted = true;
        EXPECT_EQ(call.name, "shell");
        EXPECT_NE(prompt_text.find("echo hello"), std::string::npos);
        return true;
    };

    const auto shell_result = registry.execute(ToolUseBlock{
        .id = "context-allow-shell",
        .name = "shell",
        .input = {{"command", "echo hello"}},
    });
    EXPECT_TRUE(prompted);
    EXPECT_FALSE(shell_result.is_error);
    EXPECT_NE(shell_result.content.find("hello"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, BlockedShellCommandsAreRejectedByPolicy) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::allow;
    permissions.denied_shell_commands = {"rm -rf", "shutdown"};

    (void)register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);

    const auto shell_result = registry.execute(ToolUseBlock{
        .id = "blocked-shell",
        .name = "shell",
        .input = {{"command", "rm -rf build"}},
    });
    EXPECT_TRUE(shell_result.is_error);
    EXPECT_NE(shell_result.content.find("matched 'rm -rf'"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, ScriptToolsRespectShellApprovalPolicy) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::deny;

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    (void)register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions);

    const auto result = registry.execute(ToolUseBlock{
        .id = "deny-script-shell",
        .name = "echo_custom",
        .input = json::object(),
    });
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("approval policy"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, ScriptToolsRespectDeniedShellCommands) {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::allow;
    permissions.denied_shell_commands = {"rm -rf"};

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "wipe",
        .description = "Dangerous command",
        .command = "rm -rf ${path}",
        .input_schema = {{"path", "string"}},
    }};

    (void)register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions);

    const auto result = registry.execute(ToolUseBlock{
        .id = "deny-script-command",
        .name = "wipe",
        .input = {{"path", "build"}},
    });
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("matched 'rm -rf'"), std::string::npos);
}

TEST(RuntimeToolLoaderTest, ScriptToolsUseDynamicApprovalCallbackFromToolContext) {
    SubagentRunStore run_store(":memory:");
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;
    auto tool_context = make_runtime_tool_context(&manager);
    bool prompted = false;

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    (void)register_runtime_tools(registry, nullptr, {}, &tool_context, custom_tools, {}, &permissions);
    tool_context.approval_callback = [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
        prompted = true;
        EXPECT_EQ(call.name, "echo_custom");
        EXPECT_NE(prompt_text.find("echo custom"), std::string::npos);
        return true;
    };

    const auto result = registry.execute(ToolUseBlock{
        .id = "allow-script-shell",
        .name = "echo_custom",
        .input = json::object(),
    });
    EXPECT_TRUE(prompted);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("custom"), std::string::npos);
}

TEST_F(ScriptToolsTest, LsListsDirectory) {
    const auto tmp_dir = test_tmp_root() / "orangutan_ls_test";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::create_directories(tmp_dir / "subdir");
    {
        std::ofstream(tmp_dir / "file.txt") << "hello";
    }

    const ToolUseBlock call{
        .id = "id_ls",
        .name = "ls",
        .input = {{"path", tmp_dir.string()}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("subdir"), std::string::npos);
    EXPECT_NE(result.content.find("file.txt"), std::string::npos);

    std::filesystem::remove_all(tmp_dir);
}

TEST_F(ScriptToolsTest, LsMissingPathReturnsError) {
    const ToolUseBlock call{
        .id = "id_ls2",
        .name = "ls",
        .input = {{"path", "/tmp/orangutan_nonexistent_dir_xyz"}},
    };
    const auto result = registry().execute(call);

    EXPECT_TRUE(result.is_error);
}

// ── grep tool (default script tool) ─────────────

TEST_F(ScriptToolsTest, GrepFindsPattern) {
    const auto tmp_dir = test_tmp_root() / "orangutan_grep_test";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream(tmp_dir / "a.txt") << "hello world\nfoo bar\n";
    }
    {
        std::ofstream(tmp_dir / "b.txt") << "nothing here\n";
    }

    const ToolUseBlock call{
        .id = "id_gr",
        .name = "grep",
        .input = {{"pattern", "hello"}, {"path", tmp_dir.string()}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("hello world"), std::string::npos);
    EXPECT_NE(result.content.find("a.txt"), std::string::npos);

    std::filesystem::remove_all(tmp_dir);
}

TEST_F(ScriptToolsTest, GrepNoMatchesReturnsError) {
    const auto tmp_dir = test_tmp_root() / "orangutan_grep_test2";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream(tmp_dir / "a.txt") << "hello\n";
    }

    const ToolUseBlock call{
        .id = "id_gr2",
        .name = "grep",
        .input = {{"pattern", "zzzznotfound"}, {"path", tmp_dir.string()}},
    };
    const auto result = registry().execute(call);

    // rg returns exit code 1 for no matches, which becomes an error in script tool
    EXPECT_TRUE(result.is_error);

    std::filesystem::remove_all(tmp_dir);
}

// ── credential scrubbing ────────────────────────

TEST_F(BuiltinToolsTest, ScrubsApiKeyInShellOutput) {
    const ToolUseBlock call{
        .id = "id_scrub",
        .name = "shell",
        .input = {{"command", "echo 'api_key: sk-ant-api03-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(result.content.find("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"), std::string::npos);
}

TEST_F(BuiltinToolsTest, ScrubsBearerToken) {
    const ToolUseBlock call{
        .id = "id_scrub2",
        .name = "shell",
        .input = {{"command", "echo 'Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.xxxxxxxxxxxxx'"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("[REDACTED]"), std::string::npos);
}

TEST_F(BuiltinToolsTest, DoesNotScrubNormalOutput) {
    const ToolUseBlock call{
        .id = "id_noscrub",
        .name = "shell",
        .input = {{"command", "echo 'hello world this is normal output'"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(result.content.find("hello world"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, RelativePathsResolveAgainstWorkspace) {
    const ToolUseBlock write_call{
        .id = "id_ws_write",
        .name = "write",
        .input = {{"path", "notes/todo.txt"}, {"content", "ship it"}},
    };
    const auto write_result = registry().execute(write_call);

    EXPECT_FALSE(write_result.is_error);
    EXPECT_TRUE(std::filesystem::exists(workspace() / "notes" / "todo.txt"));

    const ToolUseBlock read_call{
        .id = "id_ws_read",
        .name = "read",
        .input = {{"path", "notes/todo.txt"}},
    };
    const auto read_result = registry().execute(read_call);

    EXPECT_FALSE(read_result.is_error);
    EXPECT_NE(read_result.content.find("ship it"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, AbsolutePathsInsideWorkspaceRemainAllowed) {
    const auto file_path = workspace() / "notes" / "absolute.txt";
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream(file_path) << "inside sandbox\n";

    const ToolUseBlock read_call{
        .id = "id_ws_abs_read",
        .name = "read",
        .input = {{"path", file_path.string()}},
    };
    const auto read_result = registry().execute(read_call);

    EXPECT_FALSE(read_result.is_error);
    EXPECT_NE(read_result.content.find("inside sandbox"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ReadRejectsAbsolutePathOutsideWorkspace) {
    const auto outside_path = workspace().parent_path() / "orangutan_escape_read.txt";
    std::ofstream(outside_path) << "outside sandbox\n";

    const ToolUseBlock read_call{
        .id = "id_ws_escape_read",
        .name = "read",
        .input = {{"path", outside_path.string()}},
    };
    const auto read_result = registry().execute(read_call);

    EXPECT_TRUE(read_result.is_error);
    EXPECT_NE(read_result.content.find("workspace sandbox"), std::string::npos);

    std::filesystem::remove(outside_path);
}

TEST_F(BuiltinToolsWorkspaceTest, WriteRejectsTraversalOutsideWorkspace) {
    const auto outside_path = workspace().parent_path() / "orangutan_escape_write.txt";
    std::filesystem::remove(outside_path);

    const ToolUseBlock write_call{
        .id = "id_ws_escape_write",
        .name = "write",
        .input = {{"path", "../orangutan_escape_write.txt"}, {"content", "escaped"}},
    };
    const auto write_result = registry().execute(write_call);

    EXPECT_TRUE(write_result.is_error);
    EXPECT_NE(write_result.content.find("workspace sandbox"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(outside_path));
}

TEST_F(BuiltinToolsWorkspaceTest, ShellRunsInsideWorkspace) {
    const ToolUseBlock call{
        .id = "id_ws_shell",
        .name = "shell",
        .input = {{"command", "pwd"}},
    };
    const auto result = registry().execute(call);

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find(workspace().string()), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ShellBackgroundReturnsProcessAndPollsToCompletion) {
    std::filesystem::create_directories(workspace() / "ops");

    const auto start_payload = start_background_process(registry(), "printf 'tick\\n'; sleep 0.2; pwd; printf 'tock\\n'", "ops");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    EXPECT_FALSE(process_id.empty());
    EXPECT_TRUE(start_payload.at("running").get<bool>());

    const auto snapshot = wait_for_background_process(registry(), process_id);
    EXPECT_EQ(snapshot.at("status").get<std::string>(), "completed");
    EXPECT_EQ(snapshot.at("exit_code").get<int>(), 0);
    EXPECT_NE(snapshot.at("stdout").get<std::string>().find("tick"), std::string::npos);
    EXPECT_NE(snapshot.at("stdout").get<std::string>().find((workspace() / "ops").string()), std::string::npos);
    EXPECT_NE(snapshot.at("stdout").get<std::string>().find("tock"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ProcessListIncludesRunningBackgroundProcess) {
    const auto start_payload = start_background_process(registry(), "python3 -c \"import time; print('ready', flush=True); time.sleep(10)\"");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    const auto list_result = registry().execute(ToolUseBlock{
        .id = "list-background",
        .name = "process_list",
        .input = json::object(),
    });
    ASSERT_FALSE(list_result.is_error);

    const auto list_payload = json::parse(list_result.content);
    ASSERT_TRUE(list_payload.contains("processes"));
    const auto &processes = list_payload.at("processes");
    const auto it = std::ranges::find_if(processes, [&](const json &process) {
        return process.at("process_id").get<std::string>() == process_id;
    });
    ASSERT_NE(it, processes.end());
    EXPECT_TRUE(it->at("running").get<bool>());

    const auto kill_result = registry().execute(ToolUseBlock{
        .id = "kill-after-list",
        .name = "process_kill",
        .input = {{"process_id", process_id}},
    });
    EXPECT_FALSE(kill_result.is_error);
}

TEST_F(BuiltinToolsWorkspaceTest, ProcessKillStopsBackgroundProcess) {
    const auto start_payload = start_background_process(registry(), "python3 -c \"import time; time.sleep(10)\"");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    const auto kill_result = registry().execute(ToolUseBlock{
        .id = "kill-background",
        .name = "process_kill",
        .input = {{"process_id", process_id}},
    });
    ASSERT_FALSE(kill_result.is_error);

    const auto kill_payload = json::parse(kill_result.content);
    EXPECT_FALSE(kill_payload.at("running").get<bool>());
    EXPECT_TRUE(kill_payload.at("kill_requested").get<bool>());
    EXPECT_NE(kill_payload.at("status").get<std::string>(), "running");
    EXPECT_FALSE(kill_payload.at("signal_number").is_null());
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchRejectsPathsOutsideWorkspace) {
    const auto outside_path = workspace().parent_path() / "orangutan_escape_patch.cpp";
    std::ofstream(outside_path) << "outside\n";

    const std::string patch = "*** ../orangutan_escape_patch.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "outside\n"
                              "=======\n"
                              "still outside\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_escape", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("workspace sandbox"), std::string::npos);

    std::ifstream ifs(outside_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "outside\n");

    std::filesystem::remove(outside_path);
}

TEST_F(BuiltinToolsWorkspaceConfigAccessTest, ReadAllowsOrangutanConfigOutsideWorkspace) {
    const auto config_path = home() / ".orangutan" / "config.toml";
    std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

    const auto result = registry().execute({
        .id = "cfg_read",
        .name = "read",
        .input = {{"path", "~/.orangutan/config.toml"}},
    });

    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("model = \"claude\""), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceConfigAccessTest, WriteAllowsOrangutanConfigOutsideWorkspace) {
    const auto config_path = home() / ".orangutan" / "config.toml";

    const auto result = registry().execute({
        .id = "cfg_write",
        .name = "write",
        .input = {{"path", "~/.orangutan/config.toml"}, {"content", "[agent]\nmodel = \"gpt\"\n"}},
    });

    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "[agent]\nmodel = \"gpt\"\n");
}

TEST_F(BuiltinToolsWorkspaceConfigAccessTest, EditAllowsOrangutanConfigOutsideWorkspace) {
    const auto config_path = home() / ".orangutan" / "config.toml";
    std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

    const std::string patch = "*** ~/.orangutan/config.toml\n"
                              "<<<<<<< SEARCH\n"
                              "model = \"claude\"\n"
                              "=======\n"
                              "model = \"gpt\"\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({
        .id = "cfg_edit",
        .name = "edit",
        .input = {{"patch", patch}},
    });

    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "[agent]\nmodel = \"gpt\"\n");
}

TEST_F(BuiltinToolsWorkspaceConfigAccessTest, HomeFilesOutsideOrangutanConfigRemainBlocked) {
    const auto other_home_file = home() / "notes.txt";
    std::ofstream(other_home_file) << "private\n";

    const auto result = registry().execute({
        .id = "cfg_block",
        .name = "read",
        .input = {{"path", "~/notes.txt"}},
    });

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("workspace sandbox"), std::string::npos);
}

// ── edit tool (patch-based) ─────────────────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchSingleFileOneHunk) {
    std::ofstream(workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

    const std::string patch = "*** target.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "    return 0;\n"
                              "=======\n"
                              "    return 42;\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap1", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("1 hunk"), std::string::npos);

    std::ifstream ifs(workspace() / "target.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "int main() {\n    return 42;\n}\n");
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchSingleFileMultiHunk) {
    std::ofstream(workspace() / "multi.cpp") << "AAA\nBBB\nCCC\n";

    const std::string patch = "*** multi.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "AAA\n"
                              "=======\n"
                              "XXX\n"
                              ">>>>>>> REPLACE\n"
                              "<<<<<<< SEARCH\n"
                              "CCC\n"
                              "=======\n"
                              "ZZZ\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap2", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("2 hunks"), std::string::npos);

    std::ifstream ifs(workspace() / "multi.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "XXX\nBBB\nZZZ\n");
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchMultiFile) {
    std::ofstream(workspace() / "a.cpp") << "alpha\n";
    std::ofstream(workspace() / "b.cpp") << "beta\n";

    const std::string patch = "*** a.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "alpha\n"
                              "=======\n"
                              "ALPHA\n"
                              ">>>>>>> REPLACE\n"
                              "*** b.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "beta\n"
                              "=======\n"
                              "BETA\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap3", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("2 files"), std::string::npos);

    {
        std::ifstream ifs(workspace() / "a.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "ALPHA\n");
    }
    {
        std::ifstream ifs(workspace() / "b.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "BETA\n");
    }
}

// ── Parse error tests ───────────────────────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchEmptyPatchReturnsError) {
    const auto result = registry().execute({.id = "ap_e1", .name = "edit", .input = {{"patch", ""}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("empty"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchMissingSeparatorReturnsError) {
    const std::string patch = "*** file.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "hello\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_e2", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("separator"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchNoFileHeaderReturnsError) {
    const std::string patch = "<<<<<<< SEARCH\n"
                              "hello\n"
                              "=======\n"
                              "world\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_e3", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("file header"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchUnclosedHunkReturnsError) {
    const std::string patch = "*** file.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "hello\n"
                              "=======\n"
                              "world\n";

    const auto result = registry().execute({.id = "ap_e4", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("unclosed"), std::string::npos);
}

// ── Validation error tests ──────────────────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchSearchNotFoundReturnsError) {
    std::ofstream(workspace() / "v1.cpp") << "existing content\n";

    const std::string patch = "*** v1.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "nonexistent\n"
                              "=======\n"
                              "replacement\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_v1", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("not found"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchSearchMatchesMultipleReturnsError) {
    std::ofstream(workspace() / "v2.cpp") << "dup\ndup\n";

    const std::string patch = "*** v2.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "dup\n"
                              "=======\n"
                              "unique\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_v2", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("multiple"), std::string::npos);
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchFileNotFoundReturnsError) {
    const std::string patch = "*** does_not_exist.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "something\n"
                              "=======\n"
                              "other\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_v3", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("not found"), std::string::npos);
}

// ── Atomicity test ──────────────────────────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchAtomicRollbackOnFailure) {
    std::ofstream(workspace() / "good.cpp") << "good content\n";
    std::ofstream(workspace() / "bad.cpp") << "bad content\n";

    const std::string patch = "*** good.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "good content\n"
                              "=======\n"
                              "modified content\n"
                              ">>>>>>> REPLACE\n"
                              "*** bad.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "NONEXISTENT TEXT\n"
                              "=======\n"
                              "something\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_atom", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);

    // Verify good.cpp was NOT modified (atomic: neither file touched)
    std::ifstream ifs(workspace() / "good.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "good content\n");
}

// ── New file creation tests ─────────────────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchCreatesNewFile) {
    const std::string patch = "*** newdir/newfile.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "=======\n"
                              "#include <iostream>\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_new", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_TRUE(std::filesystem::exists(workspace() / "newdir" / "newfile.cpp"));

    std::ifstream ifs(workspace() / "newdir" / "newfile.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "#include <iostream>");
}

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchNewFileAlreadyExistsReturnsError) {
    std::ofstream(workspace() / "exists.cpp") << "already here\n";

    const std::string patch = "*** exists.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "=======\n"
                              "new content\n"
                              ">>>>>>> REPLACE\n";

    const auto result = registry().execute({.id = "ap_dup", .name = "edit", .input = {{"patch", patch}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("already exists"), std::string::npos);
}

// ── Integration: registered in registry ─────────

TEST_F(BuiltinToolsWorkspaceTest, ApplyPatchRegisteredInRegistry) {
    const auto defs = registry().definitions();
    bool found = false;
    for (const auto &def : defs) {
        if (def.name == "edit") {
            found = true;
            EXPECT_FALSE(def.description.empty());
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ── HashlineToolsTest ────────────────────────────────

class HashlineToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        workspace_ = test_tmp_root() / "orangutan_hashline_test";
        std::filesystem::remove_all(workspace_);
        std::filesystem::create_directories(workspace_);
        // Explicitly pass "hashline" — the default is "search_replace"
        register_builtin_tools(registry_, nullptr, workspace_.string(), nullptr, nullptr, "hashline");
    }

    void TearDown() override {
        tmp_env_.reset();
        std::filesystem::remove_all(workspace_);
    }

    [[nodiscard]]
    const std::filesystem::path &workspace() const {
        return workspace_;
    }
    [[nodiscard]]
    ToolRegistry &registry() {
        return registry_;
    }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path workspace_;
    ToolRegistry registry_;
};

TEST_F(HashlineToolsTest, ReadOutputHasHashTags) {
    std::ofstream(workspace() / "test.txt") << "hello world\nfoo bar\n";

    const auto result = registry().execute({.id = "r1", .name = "read", .input = {{"path", "test.txt"}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("1#"), std::string::npos);
    EXPECT_NE(result.content.find(":hello world"), std::string::npos);
    EXPECT_NE(result.content.find("2#"), std::string::npos);
    EXPECT_NE(result.content.find(":foo bar"), std::string::npos);
}

TEST_F(HashlineToolsTest, ReadWithOffsetUsesOriginalLineNumbers) {
    std::ofstream(workspace() / "offset.txt") << "line1\nline2\nline3\nline4\n";

    const auto result = registry().execute({.id = "r2", .name = "read", .input = {{"path", "offset.txt"}, {"offset", 3}, {"limit", 1}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("3#"), std::string::npos);
    EXPECT_NE(result.content.find(":line3"), std::string::npos);
}

TEST_F(HashlineToolsTest, ReadMultiPathHasHashTags) {
    std::ofstream(workspace() / "a.txt") << "aaa\n";
    std::ofstream(workspace() / "b.txt") << "bbb\n";

    const auto result = registry().execute({.id = "r3", .name = "read", .input = {{"paths", json::array({"a.txt", "b.txt"})}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("=== "), std::string::npos);
    EXPECT_NE(result.content.find("1#"), std::string::npos);
}

// ── Hashline edit tests ──────────────────────────────

TEST_F(HashlineToolsTest, EditReplaceSingleLine) {
    std::ofstream(workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

    auto hash = orangutan::compute_line_hash("    return 0;", 2);
    std::string anchor = "2#" + hash;

    json edits = json::array({{{"op", "replace"}, {"anchor", anchor}, {"content", json::array({"    return 42;"})}}});

    const auto result = registry().execute({.id = "e1", .name = "edit", .input = {{"path", "target.cpp"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("Applied"), std::string::npos);

    std::ifstream ifs(workspace() / "target.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "int main() {\n    return 42;\n}\n");
}

TEST_F(HashlineToolsTest, EditPreservesMissingFinalNewline) {
    std::ofstream(workspace() / "noeol.txt") << "aaa\nbbb";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", json::array({"ccc"})}}});

    const auto result = registry().execute({.id = "e1b", .name = "edit", .input = {{"path", "noeol.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "noeol.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nccc");
}

TEST_F(HashlineToolsTest, EditDeleteLine) {
    std::ofstream(workspace() / "del.txt") << "aaa\nbbb\nccc\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    json edits = json::array({{{"op", "delete"}, {"anchor", "2#" + hash}}});

    const auto result = registry().execute({.id = "e2", .name = "edit", .input = {{"path", "del.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "del.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nccc\n");
}

TEST_F(HashlineToolsTest, EditInsertAfterEOF) {
    std::ofstream(workspace() / "ins.txt") << "aaa\nbbb\n";

    json edits = json::array({{{"op", "insert_after"}, {"content", json::array({"ccc"})}}});

    const auto result = registry().execute({.id = "e3", .name = "edit", .input = {{"path", "ins.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "ins.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nbbb\nccc\n");
}

TEST_F(HashlineToolsTest, EditHashMismatchReturnsErrorWithContext) {
    std::ofstream(workspace() / "stale.txt") << "aaa\nbbb\nccc\n";

    json edits = json::array({{{"op", "replace"}, {"anchor", "2#ZZ"}, {"content", json::array({"XXX"})}}});

    auto actual_hash = orangutan::compute_line_hash("bbb", 2);
    if (actual_hash == "ZZ") {
        GTEST_SKIP() << "Hash collision";
    }

    const auto result = registry().execute({.id = "e4", .name = "edit", .input = {{"path", "stale.txt"}, {"edits", edits}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("mismatch"), std::string::npos);
    EXPECT_NE(result.content.find(actual_hash), std::string::npos);
}

TEST_F(HashlineToolsTest, EditContentAsStringIsSplitOnNewlines) {
    std::ofstream(workspace() / "str.txt") << "aaa\nbbb\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", "line1\nline2"}}});

    const auto result = registry().execute({.id = "e5", .name = "edit", .input = {{"path", "str.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "str.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nline1\nline2\n");
}

TEST_F(HashlineToolsTest, EditMissingAnchorForReplaceReturnsError) {
    std::ofstream(workspace() / "missing.txt") << "aaa\n";

    json edits = json::array({{{"op", "replace"}, {"content", json::array({"XXX"})}}});

    const auto result = registry().execute({.id = "e6", .name = "edit", .input = {{"path", "missing.txt"}, {"edits", edits}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("requires"), std::string::npos);
}

// ── Mode switching: schema differs between modes ──

TEST_F(HashlineToolsTest, EditToolDescriptionMentionsHashAnchors) {
    const auto defs = registry().definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            EXPECT_NE(def.description.find("hash"), std::string::npos);
            EXPECT_TRUE(def.input_schema.contains("properties"));
            EXPECT_TRUE(def.input_schema["properties"].contains("edits"));
            return;
        }
    }
    FAIL() << "edit tool not found in registry";
}

TEST(ModeSwitch, SearchReplaceEditToolDescriptionMentionsPatch) {
    ToolRegistry sr_registry;
    auto sr_workspace = orangutan::testing::test_tmp_root() / "orangutan_sr_mode_test";
    std::filesystem::create_directories(sr_workspace);
    register_builtin_tools(sr_registry, nullptr, sr_workspace.string(), nullptr, nullptr, "search_replace");

    const auto defs = sr_registry.definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            EXPECT_NE(def.description.find("patch"), std::string::npos);
            EXPECT_TRUE(def.input_schema.contains("properties"));
            EXPECT_TRUE(def.input_schema["properties"].contains("patch"));
            std::filesystem::remove_all(sr_workspace);
            return;
        }
    }
    std::filesystem::remove_all(sr_workspace);
    FAIL() << "edit tool not found in registry";
}
