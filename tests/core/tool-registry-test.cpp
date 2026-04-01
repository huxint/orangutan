#include "types/types.hpp"
#include "infra/config/config.hpp"
#include "features/tools/runtime/runtime-loader.hpp"
#include "tools/registry/tool.hpp"
#include "features/tools/script/script-loader.hpp"
#include "features/tools/core/hashline.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "infra/storage/session-store.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace orangutan;
using orangutan::testing::test_tmp_root;

namespace {

    nlohmann::json start_background_process(ToolRegistry &registry, const std::string &command, const std::string &working_dir = {}) {
        nlohmann::json input = {
            {"command", command},
            {"background", true},
        };
        if (!working_dir.empty()) {
            input["working_dir"] = working_dir;
        }

        const auto result = registry.execute(ToolUse("background-shell", "shell", std::move(input)));
        CHECK_FALSE((result.is_error));
        if (result.is_error) {
            return {};
        }

        return nlohmann::json::parse(result.content);
    }

    nlohmann::json wait_for_background_process(ToolRegistry &registry, const std::string &process_id, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        nlohmann::json last_snapshot;

        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = registry.execute(ToolUse("poll-background", "process_poll", {{"process_id", process_id}}));
            CHECK_FALSE((result.is_error));
            if (result.is_error) {
                return {};
            }

            last_snapshot = nlohmann::json::parse(result.content);
            if (!last_snapshot.value("running", true)) {
                return last_snapshot;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        INFO("background process did not finish in time: " << process_id);
        FAIL();
        return last_snapshot;
    }

    ToolRuntimeContext make_runtime_tool_context(SubagentManager *manager, std::string *current_session_id = nullptr,
                                                 std::vector<std::string> allowed_child_agents = {"reviewer"}) {
        return ToolRuntimeContext{
            .runtime_key = "runtime:cli:default",
            .agent_key = "default",
            .scope_key = "scope:parent",
            .current_session_id = current_session_id,
            .allowed_child_agents = std::move(allowed_child_agents),
            .subagent_manager = manager,
            .runtime_origin = base::origin::cli,
            .raw_caller_id = "cli:local",
        };
    }

    using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

} // namespace

// ── ToolRegistry basics ─────────────────────────

TEST_CASE("StartsEmpty") {
    const ToolRegistry registry;
    CHECK(registry.definitions().empty());
};

TEST_CASE("RegisterAndRetrieveDefinition") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "echo", .description = "Echoes input", .input_schema = {{"type", "object"}}}, .execute = [](const nlohmann::json &input) {
                                return input.at("text").get<std::string>();
                            }});

    const auto defs = registry.definitions();
    REQUIRE((defs.size()) == (1));
    CHECK(defs[0].name == "echo");
    CHECK(defs[0].description == "Echoes input");
};

TEST_CASE("ExecutesRegisteredTool") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "greet", .description = "Greets"}, .execute = [](const nlohmann::json &input) {
                                return "Hello, " + input.at("name").get<std::string>() + "!";
                            }});

    const ToolUse call("id_1", "greet", {{"name", "Alice"}});
    const auto result = registry.execute(call);

    CHECK(result.tool_use_id == "id_1");
    CHECK(result.content == "Hello, Alice!");
    CHECK(not(result.is_error));
};

TEST_CASE("UnknownToolReturnsError") {
    const ToolRegistry registry;
    const ToolUse call("id_2", "nonexistent", {});
    const auto result = registry.execute(call);

    CHECK(result.tool_use_id == "id_2");
    CHECK(result.is_error);
};

TEST_CASE("ToolExceptionBecomesError") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "boom", .description = "Always fails"}, .execute = [](const nlohmann::json &) -> std::string {
                                throw std::runtime_error("kaboom");
                            }});

    const ToolUse call("id_3", "boom", {});
    const auto result = registry.execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("kaboom"));
};

TEST_CASE("DuplicateRegistrationReplacesExistingExecutor") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "echo", .description = "First"}, .execute = [](const nlohmann::json &) {
                                return std::string{"first"};
                            }});
    registry.register_tool({.definition = {.name = "echo", .description = "Second"}, .execute = [](const nlohmann::json &) {
                                return std::string{"second"};
                            }});

    const auto defs = registry.definitions();
    REQUIRE((defs.size()) == (1));
    CHECK(defs[0].description == "Second");

    const auto result = registry.execute(ToolUse("id_dup", "echo", {}));
    CHECK(not(result.is_error));
    CHECK(result.content == "second");
};

// ── Built-in tools ──────────────────────────────

class BuiltinToolsTest {
public:
    BuiltinToolsTest() {
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

class BuiltinToolsWorkspaceTest {
public:
    BuiltinToolsWorkspaceTest() {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        workspace_ = test_tmp_root() / "orangutan_tool_workspace_test";
        std::filesystem::remove_all(workspace_);
        std::filesystem::create_directories(workspace_);
        register_builtin_tools(registry_, nullptr, workspace_.string());
    }

    ~BuiltinToolsWorkspaceTest() {
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

class BuiltinToolsWorkspaceConfigAccessTest {
public:
    BuiltinToolsWorkspaceConfigAccessTest() {
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

    ~BuiltinToolsWorkspaceConfigAccessTest() {
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

TEST_CASE("RegistersExpectedCoreTools") {
    BuiltinToolsTest fixture;
    const auto defs = fixture.registry().definitions();
    CHECK(defs.size() == 7);
    CHECK(orangutan::testing::has_tool_named(defs, "shell"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_list"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_poll"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_kill"));
    CHECK(orangutan::testing::has_tool_named(defs, "read"));
    CHECK(orangutan::testing::has_tool_named(defs, "write"));
    CHECK(orangutan::testing::has_tool_named(defs, "edit"));
};

TEST_CASE("DoesNotRegisterSubagentToolsWithoutRuntimeContext") {
    BuiltinToolsTest fixture;
    const auto defs = fixture.registry().definitions();

    for (const auto &def : defs) {
        CHECK(def.name != "subagent_spawn");
        CHECK(def.name != "subagent_status");
        CHECK(def.name != "subagent_wait");
    }
};

TEST_CASE("DoesNotRegisterMemoryToolsWithoutMemoryStore") {
    BuiltinToolsTest fixture;
    const auto defs = fixture.registry().definitions();

    CHECK(not(orangutan::testing::has_tool_named(defs, "remember")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "recall")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "forget")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_store")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_recall")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_forget")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_update")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_list")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "memory_stats")));
};

TEST_CASE("ShellRunsSimpleCommand") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_sh", "shell", {{"command", "echo hello"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("hello"));
};

TEST_CASE("ShellReportsNonZeroExitCode") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_sh2", "shell", {{"command", "false"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("exit code"));
};

TEST_CASE("ReadFileReturnsLineNumberedContents") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "aaa\nbbb\nccc\n";
    }

    const ToolUse call("id_rf", "read", {{"path", tmp.string()}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    // cat -n format: right-aligned 6-char field, tab, content
    CHECK(result.content.contains("1\taaa\n"));
    CHECK(result.content.contains("2\tbbb\n"));
    CHECK(result.content.contains("3\tccc\n"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadFileMissingReturnsError") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_rf2", "read", {{"path", "/tmp/orangutan_nonexistent_file_xyz.txt"}});
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("not found"));
};

// ── write tool ──────────────────────────────────

TEST_CASE("WriteCreatesFile") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_write_test.txt";
    std::filesystem::remove(tmp); // ensure clean state

    const ToolUse call("id_wf", "write", {{"path", tmp.string()}, {"content", "hello world"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("11 bytes"));

    // Verify file contents
    std::ifstream ifs(tmp);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "hello world");

    std::filesystem::remove(tmp);
};

TEST_CASE("WriteCreatesParentDirectories") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_test_dir" / "sub" / "file.txt";
    std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");

    const ToolUse call("id_wf2", "write", {{"path", tmp.string()}, {"content", "nested"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(std::filesystem::exists(tmp));

    std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");
};

// ── read tool — offset/limit ────────────────────

TEST_CASE("ReadFileWithOffset") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_offset_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 20; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUse call("id_off", "read", {{"path", tmp.string()}, {"offset", 10}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("10\tline10"));
    CHECK(result.content.contains("20\tline20"));
    // Should NOT contain lines before offset
    CHECK_FALSE(result.content.contains("\tline9\n"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadFileWithLimit") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_limit_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 100; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUse call("id_lim", "read", {{"path", tmp.string()}, {"limit", 5}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("1\tline1"));
    CHECK(result.content.contains("5\tline5"));
    // Should have truncation summary
    CHECK(result.content.contains("showing 5 of 100 lines"));
    // Should NOT contain line 6
    CHECK_FALSE(result.content.contains("\tline6\n"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadFileWithOffsetAndLimit") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_offlim_test.txt";
    {
        std::ofstream ofs(tmp);
        for (int i = 1; i <= 500; ++i) {
            ofs << "line" << i << "\n";
        }
    }

    const ToolUse call("id_ol", "read", {{"path", tmp.string()}, {"offset", 100}, {"limit", 50}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("100\tline100"));
    CHECK(result.content.contains("149\tline149"));
    CHECK(result.content.contains("showing 50 of 500 lines"));
    CHECK_FALSE(result.content.contains("\tline99\n"));
    CHECK_FALSE(result.content.contains("\tline150\n"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadFileOffsetBeyondEOF") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_eof_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "only\nthree\nlines\n";
    }

    const ToolUse call("id_eof", "read", {{"path", tmp.string()}, {"offset", 100}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("No content at offset 100"));
    CHECK(result.content.contains("file has 3 lines"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadRejectsNonPositiveOffset") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_invalid_offset_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "line1\n";
    }

    const ToolUse call("id_invalid_offset", "read", {{"path", tmp.string()}, {"offset", 0}});
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("offset"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadRejectsNonPositiveLimit") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_invalid_limit_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "line1\n";
    }

    const ToolUse call("id_invalid_limit", "read", {{"path", tmp.string()}, {"limit", 0}});
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("limit"));

    std::filesystem::remove(tmp);
};

// ── read tool — binary detection ────────────────

TEST_CASE("ReadBinaryFileReturnsMetadata") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_binary_test.bin";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        ofs << "header";
        ofs.put('\0');
        ofs << "binary data here";
    }

    const ToolUse call("id_bin", "read", {{"path", tmp.string()}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("Binary file:"));
    CHECK(result.content.contains(".bin"));
    CHECK(result.content.contains("bytes"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadTextFileReadsNormally") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_text_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "just text\nno nulls\n";
    }

    const ToolUse call("id_txt", "read", {{"path", tmp.string()}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK_FALSE(result.content.contains("Binary file:"));
    CHECK(result.content.contains("just text"));

    std::filesystem::remove(tmp);
};

// ── read tool — multi-path ──────────────────────

TEST_CASE("ReadMultipleFiles") {
    BuiltinToolsTest fixture;
    const auto tmp_a = test_tmp_root() / "orangutan_multi_a.txt";
    const auto tmp_b = test_tmp_root() / "orangutan_multi_b.txt";
    {
        std::ofstream(tmp_a) << "content_a\n";
        std::ofstream(tmp_b) << "content_b\n";
    }

    const ToolUse call("id_multi", "read", {{"paths", nlohmann::json::array({tmp_a.string(), tmp_b.string()})}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("=== " + tmp_a.string() + " ==="));
    CHECK(result.content.contains("=== " + tmp_b.string() + " ==="));
    CHECK(result.content.contains("content_a"));
    CHECK(result.content.contains("content_b"));

    std::filesystem::remove(tmp_a);
    std::filesystem::remove(tmp_b);
};

TEST_CASE("ReadMultiPathOneFailsContinuesOthers") {
    BuiltinToolsTest fixture;
    const auto tmp = test_tmp_root() / "orangutan_multi_ok.txt";
    {
        std::ofstream(tmp) << "good content\n";
    }

    const ToolUse call("id_mpf", "read", {{"paths", nlohmann::json::array({tmp.string(), "/tmp/orangutan_nonexistent_xyz.txt"})}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("good content"));
    CHECK(result.content.contains("Error:"));

    std::filesystem::remove(tmp);
};

TEST_CASE("ReadBothPathAndPathsReturnsError") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_both", "read", {{"path", "/tmp/a.txt"}, {"paths", nlohmann::json::array({"/tmp/b.txt"})}});
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("not both"));
};

TEST_CASE("ReadNeitherPathNorPathsReturnsError") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_neither", "read", nlohmann::json::object());
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
    CHECK(result.content.contains("Required"));
};

// ── user-selected script tools ───────────────────

class ScriptToolsTest {
public:
    ScriptToolsTest() {
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

TEST_CASE("RegistersBuiltinAndCustomToolsOnly") {
    ToolRegistry registry;
    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "printf custom",
    }};

    const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {});
    const auto defs = registry.definitions();

    CHECK(result.mcp_tool_count == 0);
    CHECK(result.mcp_manager == nullptr);
    CHECK(orangutan::testing::has_tool_named(defs, "shell"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_list"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_poll"));
    CHECK(orangutan::testing::has_tool_named(defs, "process_kill"));
    CHECK(orangutan::testing::has_tool_named(defs, "read"));
    CHECK(orangutan::testing::has_tool_named(defs, "write"));
    CHECK(orangutan::testing::has_tool_named(defs, "edit"));
    CHECK(not(orangutan::testing::has_tool_named(defs, "ls")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "grep")));
    CHECK(orangutan::testing::has_tool_named(defs, "echo_custom"));
};

TEST_CASE("RegistersUsableMemoryAndSubagentToolsTogether") {
    ToolRegistry registry;
    // Real child execution is covered in SubagentIntegrationTest; this regression test only checks that runtime bootstrap wires usable memory and subagent control tools
    // together in one registry.
    const auto memory_db = test_tmp_root() / "orangutan_runtime_tool_loader_memory.db";
    const auto session_db = test_tmp_root() / "orangutan_runtime_tool_loader_sessions.db";
    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);

    {
        MemoryStore memory_store(memory_db);
        RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{.scope = "scope:parent"});
        SessionStore session_store(session_db);
        const auto parent_session_id =
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        const auto child_session_id =
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:child", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});

        {
            SubagentRunStore run_store(session_db);
            SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
                return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
            });
            auto current_session_id = parent_session_id;
            const auto tool_context = make_runtime_tool_context(&manager, &current_session_id);

            const auto result = register_runtime_tools(registry, &runtime_memory, {}, &tool_context, {}, {});
            const auto defs = registry.definitions();

            CHECK(result.mcp_tool_count == 0);
            CHECK(result.mcp_manager == nullptr);
            CHECK(orangutan::testing::has_tool_named(defs, "shell"));
            CHECK(orangutan::testing::has_tool_named(defs, "process_list"));
            CHECK(orangutan::testing::has_tool_named(defs, "process_poll"));
            CHECK(orangutan::testing::has_tool_named(defs, "process_kill"));
            CHECK(orangutan::testing::has_tool_named(defs, "read"));
            CHECK(not(orangutan::testing::has_tool_named(defs, "ls")));
            CHECK(not(orangutan::testing::has_tool_named(defs, "grep")));
            CHECK(orangutan::testing::has_tool_named(defs, "subagent_spawn"));
            CHECK(orangutan::testing::has_tool_named(defs, "subagent_status"));
            CHECK(orangutan::testing::has_tool_named(defs, "subagent_wait"));
            CHECK(orangutan::testing::has_tool_named(defs, "remember"));
            CHECK(orangutan::testing::has_tool_named(defs, "memory_recall"));
            CHECK(orangutan::testing::has_tool_named(defs, "memory_stats"));

            const auto remember = registry.execute(ToolUse("remember-runtime", "remember", {{"key", "theme"}, {"content", "blue"}, {"category", "prefs"}}));
            CHECK(not(remember.is_error));

            const auto recall = registry.execute(ToolUse("recall-runtime", "memory_recall", {{"query", "theme"}}));
            CHECK(not(recall.is_error));
            CHECK(recall.content.contains("blue"));

            const auto spawn = registry.execute(ToolUse("spawn-runtime", "subagent_spawn",
                                                        {{"child_agent_key", "reviewer"},
                                                         {"child_scope_key", "scope:child"},
                                                         {"child_session_id", child_session_id},
                                                         {"task_summary", "Review the tool layout refactor"}}));
            CHECK(not(spawn.is_error));
            const auto spawn_payload = nlohmann::json::parse(spawn.content);
            REQUIRE(spawn_payload.at("accepted").get<bool>());
            const auto run_id = spawn_payload.at("run_id").get<std::string>();
            CHECK(not(run_id.empty()));

            const auto wait = registry.execute(ToolUse("wait-runtime", "subagent_wait", {{"run_id", run_id}, {"timeout_ms", 1000}}));
            CHECK(not(wait.is_error));
            const auto wait_payload = nlohmann::json::parse(wait.content);
            CHECK(wait_payload.at("state").get<std::string>() == "completed");
        }
    }

    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);
};

TEST_CASE("DeniedToolsAreHiddenAndBlockedByPolicy") {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.denied_tools = {"shell"};

    const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);
    const auto defs = registry.definitions();

    CHECK(result.mcp_tool_count == 0);
    CHECK(result.mcp_manager == nullptr);
    CHECK(not(orangutan::testing::has_tool_named(defs, "shell")));
    CHECK(orangutan::testing::has_tool_named(defs, "read"));

    const auto shell_result = registry.execute(ToolUse("deny-shell", "shell", {{"command", "echo hello"}}));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("permission policy"));
};

TEST_CASE("ShellApprovalAskBlocksWhenPromptUnavailable") {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("ask-shell", "shell", {{"command", "echo hello"}}));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("requires approval"));
};

TEST_CASE("ShellApprovalCallbackCanAllowCommand") {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::ask;

    bool prompted = false;
    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions, [&prompted](const ToolUse &call, const std::string &prompt_text) {
        prompted = true;
        CHECK(call.name == "shell");
        CHECK(prompt_text.contains("echo hello"));
        return true;
    }));

    const auto shell_result = registry.execute(ToolUse("allow-shell", "shell", {{"command", "echo hello"}}));
    CHECK(prompted);
    CHECK(not(shell_result.is_error));
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("UsesDynamicApprovalCallbackFromToolContext") {
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

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));
    tool_context.approval_callback = [&prompted](const ToolUse &call, const std::string &prompt_text) {
        prompted = true;
        CHECK(call.name == "shell");
        CHECK(prompt_text.contains("echo hello"));
        return true;
    };

    const auto shell_result = registry.execute(ToolUse("context-allow-shell", "shell", {{"command", "echo hello"}}));
    CHECK(prompted);
    CHECK(not(shell_result.is_error));
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("BlockedShellCommandsAreRejectedByPolicy") {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::allow;
    permissions.denied_shell_commands = {"rm -rf", "shutdown"};

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("blocked-shell", "shell", {{"command", "rm -rf build"}}));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("matched 'rm -rf'"));
};

TEST_CASE("ScriptToolsRespectShellApprovalPolicy") {
    ToolRegistry registry;
    ToolPermissionSettings permissions;
    permissions.sandbox_mode = ToolSandboxMode::disabled;
    permissions.shell_approval = ToolApprovalPolicy::deny;

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions));

    const auto result = registry.execute(ToolUse("deny-script-shell", "echo_custom", nlohmann::json::object()));
    CHECK(result.is_error);
    CHECK(result.content.contains("approval policy"));
};

TEST_CASE("ScriptToolsRespectDeniedShellCommands") {
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

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions));

    const auto result = registry.execute(ToolUse("deny-script-command", "wipe", {{"path", "build"}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("matched 'rm -rf'"));
};

TEST_CASE("ScriptToolsUseDynamicApprovalCallbackFromToolContext") {
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

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, custom_tools, {}, &permissions));
    tool_context.approval_callback = [&prompted](const ToolUse &call, const std::string &prompt_text) {
        prompted = true;
        CHECK(call.name == "echo_custom");
        CHECK(prompt_text.contains("echo custom"));
        return true;
    };

    const auto result = registry.execute(ToolUse("allow-script-shell", "echo_custom", nlohmann::json::object()));
    CHECK(prompted);
    CHECK(not(result.is_error));
    CHECK(result.content.contains("custom"));
};

TEST_CASE("LsListsDirectory") {
    ScriptToolsTest fixture;
    const auto tmp_dir = test_tmp_root() / "orangutan_ls_test";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::create_directories(tmp_dir / "subdir");
    {
        std::ofstream(tmp_dir / "file.txt") << "hello";
    }

    const ToolUse call("id_ls", "ls", {{"path", tmp_dir.string()}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("subdir"));
    CHECK(result.content.contains("file.txt"));

    std::filesystem::remove_all(tmp_dir);
};

TEST_CASE("LsMissingPathReturnsError") {
    ScriptToolsTest fixture;
    const ToolUse call("id_ls2", "ls", {{"path", "/tmp/orangutan_nonexistent_dir_xyz"}});
    const auto result = fixture.registry().execute(call);

    CHECK(result.is_error);
};

// ── grep tool (default script tool) ─────────────

TEST_CASE("GrepFindsPattern") {
    ScriptToolsTest fixture;
    const auto tmp_dir = test_tmp_root() / "orangutan_grep_test";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream(tmp_dir / "a.txt") << "hello world\nfoo bar\n";
    }
    {
        std::ofstream(tmp_dir / "b.txt") << "nothing here\n";
    }

    const ToolUse call("id_gr", "grep", {{"pattern", "hello"}, {"path", tmp_dir.string()}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("hello world"));
    CHECK(result.content.contains("a.txt"));

    std::filesystem::remove_all(tmp_dir);
};

TEST_CASE("GrepNoMatchesReturnsError") {
    ScriptToolsTest fixture;
    const auto tmp_dir = test_tmp_root() / "orangutan_grep_test2";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream(tmp_dir / "a.txt") << "hello\n";
    }

    const ToolUse call("id_gr2", "grep", {{"pattern", "zzzznotfound"}, {"path", tmp_dir.string()}});
    const auto result = fixture.registry().execute(call);

    // rg returns exit code 1 for no matches, which becomes an error in script tool
    CHECK(result.is_error);

    std::filesystem::remove_all(tmp_dir);
};

// ── credential scrubbing ────────────────────────

TEST_CASE("ScrubsApiKeyInShellOutput") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_scrub", "shell", {{"command", "echo 'api_key: sk-ant-api03-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("[REDACTED]"));
    CHECK_FALSE(result.content.contains("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
};

TEST_CASE("ScrubsBearerToken") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_scrub2", "shell", {{"command", "echo 'Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.xxxxxxxxxxxxx'"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains("[REDACTED]"));
};

TEST_CASE("DoesNotScrubNormalOutput") {
    BuiltinToolsTest fixture;
    const ToolUse call("id_noscrub", "shell", {{"command", "echo 'hello world this is normal output'"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK_FALSE(result.content.contains("[REDACTED]"));
    CHECK(result.content.contains("hello world"));
};

TEST_CASE("RelativePathsResolveAgainstWorkspace") {
    BuiltinToolsWorkspaceTest fixture;
    const ToolUse write_call("id_ws_write", "write", {{"path", "notes/todo.txt"}, {"content", "ship it"}});
    const auto write_result = fixture.registry().execute(write_call);

    CHECK(not(write_result.is_error));
    CHECK(std::filesystem::exists(fixture.workspace() / "notes" / "todo.txt"));

    const ToolUse read_call("id_ws_read", "read", {{"path", "notes/todo.txt"}});
    const auto read_result = fixture.registry().execute(read_call);

    CHECK(not(read_result.is_error));
    CHECK(read_result.content.contains("ship it"));
};

TEST_CASE("AbsolutePathsInsideWorkspaceRemainAllowed") {
    BuiltinToolsWorkspaceTest fixture;
    const auto file_path = fixture.workspace() / "notes" / "absolute.txt";
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream(file_path) << "inside sandbox\n";

    const ToolUse read_call("id_ws_abs_read", "read", {{"path", file_path.string()}});
    const auto read_result = fixture.registry().execute(read_call);

    CHECK(not(read_result.is_error));
    CHECK(read_result.content.contains("inside sandbox"));
};

TEST_CASE("ReadRejectsAbsolutePathOutsideWorkspace") {
    BuiltinToolsWorkspaceTest fixture;
    const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_read.txt";
    std::ofstream(outside_path) << "outside sandbox\n";

    const ToolUse read_call("id_ws_escape_read", "read", {{"path", outside_path.string()}});
    const auto read_result = fixture.registry().execute(read_call);

    CHECK(read_result.is_error);
    CHECK(read_result.content.contains("workspace sandbox"));

    std::filesystem::remove(outside_path);
};

TEST_CASE("WriteRejectsTraversalOutsideWorkspace") {
    BuiltinToolsWorkspaceTest fixture;
    const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_write.txt";
    std::filesystem::remove(outside_path);

    const ToolUse write_call("id_ws_escape_write", "write", {{"path", "../orangutan_escape_write.txt"}, {"content", "escaped"}});
    const auto write_result = fixture.registry().execute(write_call);

    CHECK(write_result.is_error);
    CHECK(write_result.content.contains("workspace sandbox"));
    CHECK(not(std::filesystem::exists(outside_path)));
};

TEST_CASE("ShellRunsInsideWorkspace") {
    BuiltinToolsWorkspaceTest fixture;
    const ToolUse call("id_ws_shell", "shell", {{"command", "pwd"}});
    const auto result = fixture.registry().execute(call);

    CHECK(not(result.is_error));
    CHECK(result.content.contains(fixture.workspace().string()));
};

TEST_CASE("ShellBackgroundReturnsProcessAndPollsToCompletion") {
    BuiltinToolsWorkspaceTest fixture;
    std::filesystem::create_directories(fixture.workspace() / "ops");

    const auto start_payload = start_background_process(fixture.registry(), "printf 'tick\\n'; sleep 0.2; pwd; printf 'tock\\n'", "ops");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    CHECK(not(process_id.empty()));
    CHECK(start_payload.at("running").get<bool>());

    const auto snapshot = wait_for_background_process(fixture.registry(), process_id);
    CHECK(snapshot.at("status").get<std::string>() == "completed");
    CHECK(snapshot.at("exit_code").get<int>() == 0);
    CHECK(snapshot.at("stdout").get<std::string>().contains("tick"));
    CHECK(snapshot.at("stdout").get<std::string>().contains((fixture.workspace() / "ops").string()));
    CHECK(snapshot.at("stdout").get<std::string>().contains("tock"));
};

TEST_CASE("ProcessListIncludesRunningBackgroundProcess") {
    BuiltinToolsWorkspaceTest fixture;
    const auto start_payload = start_background_process(fixture.registry(), "python3 -c \"import time; print('ready', flush=True); time.sleep(10)\"");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    const auto list_result = fixture.registry().execute(ToolUse("list-background", "process_list", nlohmann::json::object()));
    REQUIRE(not(list_result.is_error));

    const auto list_payload = nlohmann::json::parse(list_result.content);
    REQUIRE(list_payload.contains("processes"));
    const auto &processes = list_payload.at("processes");
    const auto it = std::ranges::find_if(processes, [&](const nlohmann::json &process) {
        return process.at("process_id").get<std::string>() == process_id;
    });
    REQUIRE((it) != (processes.end()));
    CHECK(it->at("running").get<bool>());

    const auto kill_result = fixture.registry().execute(ToolUse("kill-after-list", "process_kill", {{"process_id", process_id}}));
    CHECK(not(kill_result.is_error));
};

TEST_CASE("ProcessKillStopsBackgroundProcess") {
    BuiltinToolsWorkspaceTest fixture;
    const auto start_payload = start_background_process(fixture.registry(), "python3 -c \"import time; time.sleep(10)\"");
    const auto process_id = start_payload.at("process_id").get<std::string>();

    const auto kill_result = fixture.registry().execute(ToolUse("kill-background", "process_kill", {{"process_id", process_id}}));
    REQUIRE(not(kill_result.is_error));

    const auto kill_payload = nlohmann::json::parse(kill_result.content);
    CHECK(not(kill_payload.at("running").get<bool>()));
    CHECK(kill_payload.at("kill_requested").get<bool>());
    CHECK(kill_payload.at("status").get<std::string>() != "running");
    CHECK(not(kill_payload.at("signal_number").is_null()));
};

TEST_CASE("ApplyPatchRejectsPathsOutsideWorkspace") {
    BuiltinToolsWorkspaceTest fixture;
    const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_patch.cpp";
    std::ofstream(outside_path) << "outside\n";

    const std::string patch = "*** ../orangutan_escape_patch.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "outside\n"
                              "=======\n"
                              "still outside\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_escape", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("workspace sandbox"));

    std::ifstream ifs(outside_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "outside\n");

    std::filesystem::remove(outside_path);
};

TEST_CASE("ReadAllowsOrangutanConfigOutsideWorkspace") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto config_path = fixture.home() / ".orangutan" / "config.toml";
    std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

    const auto result = fixture.registry().execute(ToolUse("cfg_read", "read", {{"path", "~/.orangutan/config.toml"}}));

    CHECK(not(result.is_error));
    CHECK(result.content.contains("model = \"claude\""));
};

TEST_CASE("WriteAllowsOrangutanConfigOutsideWorkspace") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto config_path = fixture.home() / ".orangutan" / "config.toml";

    const auto result = fixture.registry().execute(ToolUse("cfg_write", "write", {{"path", "~/.orangutan/config.toml"}, {"content", "[agent]\nmodel = \"gpt\"\n"}}));

    CHECK(not(result.is_error));

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "[agent]\nmodel = \"gpt\"\n");
};

TEST_CASE("EditAllowsOrangutanConfigOutsideWorkspace") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto config_path = fixture.home() / ".orangutan" / "config.toml";
    std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

    const std::string patch = "*** ~/.orangutan/config.toml\n"
                              "<<<<<<< SEARCH\n"
                              "model = \"claude\"\n"
                              "=======\n"
                              "model = \"gpt\"\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("cfg_edit", "edit", {{"patch", patch}}));

    CHECK(not(result.is_error));

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "[agent]\nmodel = \"gpt\"\n");
};

TEST_CASE("HomeFilesOutsideOrangutanConfigRemainBlocked") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto other_home_file = fixture.home() / "notes.txt";
    std::ofstream(other_home_file) << "private\n";

    const auto result = fixture.registry().execute(ToolUse("cfg_block", "read", {{"path", "~/notes.txt"}}));

    CHECK(result.is_error);
    CHECK(result.content.contains("workspace sandbox"));
};

// ── edit tool (patch-based) ─────────────────────

TEST_CASE("ApplyPatchSingleFileOneHunk") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

    const std::string patch = "*** target.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "    return 0;\n"
                              "=======\n"
                              "    return 42;\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap1", "edit", {{"patch", patch}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("1 hunk"));

    std::ifstream ifs(fixture.workspace() / "target.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "int main() {\n    return 42;\n}\n");
};

TEST_CASE("ApplyPatchSingleFileMultiHunk") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "multi.cpp") << "AAA\nBBB\nCCC\n";

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

    const auto result = fixture.registry().execute(ToolUse("ap2", "edit", {{"patch", patch}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("2 hunks"));

    std::ifstream ifs(fixture.workspace() / "multi.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "XXX\nBBB\nZZZ\n");
};

TEST_CASE("ApplyPatchMultiFile") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "a.cpp") << "alpha\n";
    std::ofstream(fixture.workspace() / "b.cpp") << "beta\n";

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

    const auto result = fixture.registry().execute(ToolUse("ap3", "edit", {{"patch", patch}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("2 files"));

    {
        std::ifstream ifs(fixture.workspace() / "a.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        CHECK(content == "ALPHA\n");
    }
    {
        std::ifstream ifs(fixture.workspace() / "b.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        CHECK(content == "BETA\n");
    }
};

// ── Parse error tests ───────────────────────────

TEST_CASE("ApplyPatchEmptyPatchReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    const auto result = fixture.registry().execute(ToolUse("ap_e1", "edit", {{"patch", ""}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("empty"));
};

TEST_CASE("ApplyPatchMissingSeparatorReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    const std::string patch = "*** file.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "hello\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_e2", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("separator"));
};

TEST_CASE("ApplyPatchNoFileHeaderReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    const std::string patch = "<<<<<<< SEARCH\n"
                              "hello\n"
                              "=======\n"
                              "world\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_e3", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("file header"));
};

TEST_CASE("ApplyPatchUnclosedHunkReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    const std::string patch = "*** file.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "hello\n"
                              "=======\n"
                              "world\n";

    const auto result = fixture.registry().execute(ToolUse("ap_e4", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("unclosed"));
};

// ── Validation error tests ──────────────────────

TEST_CASE("ApplyPatchSearchNotFoundReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "v1.cpp") << "existing content\n";

    const std::string patch = "*** v1.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "nonexistent\n"
                              "=======\n"
                              "replacement\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_v1", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("not found"));
};

TEST_CASE("ApplyPatchSearchMatchesMultipleReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "v2.cpp") << "dup\ndup\n";

    const std::string patch = "*** v2.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "dup\n"
                              "=======\n"
                              "unique\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_v2", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("multiple"));
};

TEST_CASE("ApplyPatchFileNotFoundReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    const std::string patch = "*** does_not_exist.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "something\n"
                              "=======\n"
                              "other\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_v3", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("not found"));
};

// ── Atomicity test ──────────────────────────────

TEST_CASE("ApplyPatchAtomicRollbackOnFailure") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "good.cpp") << "good content\n";
    std::ofstream(fixture.workspace() / "bad.cpp") << "bad content\n";

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

    const auto result = fixture.registry().execute(ToolUse("ap_atom", "edit", {{"patch", patch}}));
    CHECK(result.is_error);

    // Verify good.cpp was NOT modified (atomic: neither file touched)
    std::ifstream ifs(fixture.workspace() / "good.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "good content\n");
};

// ── New file creation tests ─────────────────────

TEST_CASE("ApplyPatchCreatesNewFile") {
    BuiltinToolsWorkspaceTest fixture;
    const std::string patch = "*** newdir/newfile.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "=======\n"
                              "#include <iostream>\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_new", "edit", {{"patch", patch}}));
    CHECK(not(result.is_error));
    CHECK(std::filesystem::exists(fixture.workspace() / "newdir" / "newfile.cpp"));

    std::ifstream ifs(fixture.workspace() / "newdir" / "newfile.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "#include <iostream>");
};

TEST_CASE("ApplyPatchNewFileAlreadyExistsReturnsError") {
    BuiltinToolsWorkspaceTest fixture;
    std::ofstream(fixture.workspace() / "exists.cpp") << "already here\n";

    const std::string patch = "*** exists.cpp\n"
                              "<<<<<<< SEARCH\n"
                              "=======\n"
                              "new content\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("ap_dup", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("already exists"));
};

// ── Integration: registered in registry ─────────

TEST_CASE("ApplyPatchRegisteredInRegistry") {
    BuiltinToolsWorkspaceTest fixture;
    const auto defs = fixture.registry().definitions();
    bool found = false;
    for (const auto &def : defs) {
        if (def.name == "edit") {
            found = true;
            CHECK(not(def.description.empty()));
            break;
        }
    }
    CHECK(found);
};

// ── HashlineToolsTest ────────────────────────────────

class HashlineToolsTest {
public:
    HashlineToolsTest() {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        workspace_ = test_tmp_root() / "orangutan_hashline_test";
        std::filesystem::remove_all(workspace_);
        std::filesystem::create_directories(workspace_);
        // Explicitly pass "hashline" — the default is "search_replace"
        register_builtin_tools(registry_, nullptr, workspace_.string(), nullptr, nullptr, "hashline");
    }

    ~HashlineToolsTest() {
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

TEST_CASE("ReadOutputHasHashTags") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "test.txt") << "hello world\nfoo bar\n";

    const auto result = fixture.registry().execute(ToolUse("r1", "read", {{"path", "test.txt"}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("1#"));
    CHECK(result.content.contains(":hello world"));
    CHECK(result.content.contains("2#"));
    CHECK(result.content.contains(":foo bar"));
};

TEST_CASE("ReadWithOffsetUsesOriginalLineNumbers") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "offset.txt") << "line1\nline2\nline3\nline4\n";

    const auto result = fixture.registry().execute(ToolUse("r2", "read", {{"path", "offset.txt"}, {"offset", 3}, {"limit", 1}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("3#"));
    CHECK(result.content.contains(":line3"));
};

TEST_CASE("ReadMultiPathHasHashTags") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "a.txt") << "aaa\n";
    std::ofstream(fixture.workspace() / "b.txt") << "bbb\n";

    const auto result = fixture.registry().execute(ToolUse("r3", "read", {{"paths", nlohmann::json::array({"a.txt", "b.txt"})}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("=== "));
    CHECK(result.content.contains("1#"));
};

// ── Hashline edit tests ──────────────────────────────

TEST_CASE("EditReplaceSingleLine") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

    auto hash = orangutan::compute_line_hash("    return 0;", 2);
    std::string anchor = "2#" + hash;

    nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"anchor", anchor}, {"content", nlohmann::json::array({"    return 42;"})}}});

    const auto result = fixture.registry().execute(ToolUse("e1", "edit", {{"path", "target.cpp"}, {"edits", edits}}));
    CHECK(not(result.is_error));
    CHECK(result.content.contains("Applied"));

    std::ifstream ifs(fixture.workspace() / "target.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "int main() {\n    return 42;\n}\n");
};

TEST_CASE("EditPreservesMissingFinalNewline") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "noeol.txt") << "aaa\nbbb";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", nlohmann::json::array({"ccc"})}}});

    const auto result = fixture.registry().execute(ToolUse("e1b", "edit", {{"path", "noeol.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "noeol.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nccc");
};

TEST_CASE("EditDeleteLine") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "del.txt") << "aaa\nbbb\nccc\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({{{"op", "delete"}, {"anchor", "2#" + hash}}});

    const auto result = fixture.registry().execute(ToolUse("e2", "edit", {{"path", "del.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "del.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nccc\n");
};

TEST_CASE("EditInsertAfterEOF") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "ins.txt") << "aaa\nbbb\n";

    nlohmann::json edits = nlohmann::json::array({{{"op", "insert_after"}, {"content", nlohmann::json::array({"ccc"})}}});

    const auto result = fixture.registry().execute(ToolUse("e3", "edit", {{"path", "ins.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "ins.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nbbb\nccc\n");
};

TEST_CASE("EditHashMismatchReturnsErrorWithContext") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "stale.txt") << "aaa\nbbb\nccc\n";

    auto actual_hash = orangutan::compute_line_hash("bbb", 2);
    const std::string wrong_hash = actual_hash == "ZZ" ? "ZY" : "ZZ";
    nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"anchor", "2#" + wrong_hash}, {"content", nlohmann::json::array({"XXX"})}}});

    const auto result = fixture.registry().execute(ToolUse("e4", "edit", {{"path", "stale.txt"}, {"edits", edits}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("mismatch"));
    CHECK(result.content.contains(actual_hash));
};

TEST_CASE("EditContentAsStringIsSplitOnNewlines") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "str.txt") << "aaa\nbbb\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", "line1\nline2"}}});

    const auto result = fixture.registry().execute(ToolUse("e5", "edit", {{"path", "str.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "str.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nline1\nline2\n");
};

TEST_CASE("EditMissingAnchorForReplaceReturnsError") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "missing.txt") << "aaa\n";

    nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"content", nlohmann::json::array({"XXX"})}}});

    const auto result = fixture.registry().execute(ToolUse("e6", "edit", {{"path", "missing.txt"}, {"edits", edits}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("requires"));
};

// ── Mode switching: schema differs between modes ──

TEST_CASE("EditToolDescriptionMentionsHashAnchors") {
    HashlineToolsTest fixture;
    const auto defs = fixture.registry().definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            CHECK(def.description.contains("hash"));
            CHECK(def.input_schema.contains("properties"));
            CHECK(def.input_schema["properties"].contains("edits"));
            return;
        }
    }
    FAIL("edit tool not found in registry");
};

TEST_CASE("SearchReplaceEditToolDescriptionMentionsPatch") {
    ToolRegistry sr_registry;
    auto sr_workspace = orangutan::testing::test_tmp_root() / "orangutan_sr_mode_test";
    std::filesystem::create_directories(sr_workspace);
    register_builtin_tools(sr_registry, nullptr, sr_workspace.string(), nullptr, nullptr, "search_replace");

    const auto defs = sr_registry.definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            CHECK(def.description.contains("patch"));
            CHECK(def.input_schema.contains("properties"));
            CHECK(def.input_schema["properties"].contains("patch"));
            std::filesystem::remove_all(sr_workspace);
            return;
        }
    }
    std::filesystem::remove_all(sr_workspace);
    FAIL("edit tool not found in registry");
};
