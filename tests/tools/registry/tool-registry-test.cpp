#include "types/types.hpp"
#include "automation/repository.hpp"
#include "automation/service.hpp"
#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "skills/skill-loader.hpp"
#include "tools/runtime-loader/runtime-loader.hpp"
#include "tools/registry/tool.hpp"
#include "tools/script/script-loader.hpp"
#include "tools/skill/skill-tool.hpp"
#include "tools/tool-search/tool-search.hpp"
#include "tools/file/edit/hashline.hpp"
#include "tools/internal.hpp"
#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "storage/session-store.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <type_traits>

using namespace orangutan;
using namespace orangutan::tools;
using orangutan::testing::test_tmp_root;

namespace {

    static_assert(std::same_as<decltype(&register_builtin_tools), void (*)(ToolRegistry &, memory::RuntimeMemory *, const std::filesystem::path &, const ToolRuntimeContext *,
                                                                           const ToolPermissionContext *, tools::file::edit_mode)>);

    using FindDefinitionSignature = const ToolDef *(ToolRegistry::*)(std::string_view) const;
    using FindToolSignature = const Tool *(ToolRegistry::*)(std::string_view) const;

    static_assert(std::same_as<decltype(&ToolRegistry::find_definition), FindDefinitionSignature>);
    static_assert(std::same_as<decltype(&ToolRegistry::find_tool), FindToolSignature>);

    static_assert(std::same_as<decltype(&register_runtime_tools),
                               RuntimeToolBootstrapResult (*)(ToolRegistry &, memory::RuntimeMemory *, const std::filesystem::path &, const ToolRuntimeContext *,
                                                              const std::vector<Config::ScriptToolConfig> &, const std::vector<Config::McpServerConfig> &,
                                                              const ToolPermissionContext *, tools::file::edit_mode)>);

    using RegisterShellToolSignature = void (*)(ToolRegistry &, const std::filesystem::path &, const ToolPermissionContext *,
                                                const std::shared_ptr<BackgroundCompletionDispatcher> &, const std::shared_ptr<BackgroundProcessManager> &);

    static_assert(std::same_as<decltype(&register_shell_tool), RegisterShellToolSignature>);

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

    ToolRuntimeContext make_runtime_tool_context(std::string *current_session_id = nullptr) {
        return ToolRuntimeContext{
            .runtime_key = "runtime:cli:default",
            .agent_key = "default",
            .scope_key = "scope:parent",
            .current_session_id = current_session_id,
            .runtime_origin = base::origin::cli,
            .raw_caller_id = "cli:local",
        };
    }

    using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

    void write_skill_file(const std::filesystem::path &root, std::string_view dir_name, std::string_view frontmatter, std::string_view body) {
        const auto skill_dir = root / std::filesystem::path(dir_name);
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "---\n";
        out << frontmatter;
        out << "\n---\n\n";
        out << body;
        out << '\n';
    }

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
    BuiltinToolsWorkspaceTest(const BuiltinToolsWorkspaceTest &) = delete;
    BuiltinToolsWorkspaceTest &operator=(const BuiltinToolsWorkspaceTest &) = delete;
    BuiltinToolsWorkspaceTest(BuiltinToolsWorkspaceTest &&) = delete;
    BuiltinToolsWorkspaceTest &operator=(BuiltinToolsWorkspaceTest &&) = delete;

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
    BuiltinToolsWorkspaceConfigAccessTest(const BuiltinToolsWorkspaceConfigAccessTest &) = delete;
    BuiltinToolsWorkspaceConfigAccessTest &operator=(const BuiltinToolsWorkspaceConfigAccessTest &) = delete;
    BuiltinToolsWorkspaceConfigAccessTest(BuiltinToolsWorkspaceConfigAccessTest &&) = delete;
    BuiltinToolsWorkspaceConfigAccessTest &operator=(BuiltinToolsWorkspaceConfigAccessTest &&) = delete;

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

TEST_CASE("DoesNotRegisterAgentToolsWithoutRuntimeContext") {
    BuiltinToolsTest fixture;
    const auto defs = fixture.registry().definitions();

    for (const auto &def : defs) {
        CHECK(def.name != "agent_spawn");
        CHECK(def.name != "agent_send_message");
        CHECK(def.name != "agent_stop");
    }
};

TEST_CASE("DoesNotRegisterMemoryToolsWithoutMemoryStore") {
    BuiltinToolsTest fixture;
    const auto defs = fixture.registry().definitions();

    CHECK(not(orangutan::testing::has_tool_named(defs, "remember")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "recall")));
    CHECK(not(orangutan::testing::has_tool_named(defs, "forget")));
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

TEST_CASE("RegistersUsableMemoryToolsWithRuntimeContext") {
    ToolRegistry registry;
    // This regression test checks that runtime bootstrap wires usable memory tools together in one registry.
    const auto memory_db = test_tmp_root() / "orangutan_runtime_tool_loader_memory.db";
    const auto session_db = test_tmp_root() / "orangutan_runtime_tool_loader_sessions.db";
    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);

    {
        MemoryStore memory_store(memory_db);
        RuntimeMemory runtime_memory(memory_store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:parent"});
        SessionStore session_store(session_db);
        const auto parent_session_id =
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});

        {
            auto current_session_id = parent_session_id;
            const auto tool_context = make_runtime_tool_context(&current_session_id);

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
            // Memory tools are deferred — not in definitions() but still executable
            CHECK(not(orangutan::testing::has_tool_named(defs, "remember")));
            CHECK(not(orangutan::testing::has_tool_named(defs, "recall")));
            CHECK(not(orangutan::testing::has_tool_named(defs, "memory_stats")));
            CHECK(orangutan::testing::has_tool_named(defs, "tool_search"));
            CHECK(registry.has_deferred_tools());

            const auto remember = registry.execute(ToolUse("remember-runtime", "remember", {{"key", "theme"}, {"content", "blue"}, {"category", "prefs"}}));
            CHECK(not(remember.is_error));

            const auto recall = registry.execute(ToolUse("recall-runtime", "recall", {{"mode", "query"}, {"value", "theme"}}));
            CHECK(not(recall.is_error));
            CHECK(recall.content.contains("blue"));
        }
    }

    std::filesystem::remove(memory_db);
    std::filesystem::remove(session_db);
};

TEST_CASE("DeniedToolsAreHiddenAndBlockedByPolicy") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.deny_rules.push_back(PermissionRule{
        .source = permission_rule_source::cli_arg,
        .behavior = permission_behavior::deny,
        .tool_name = "shell",
    });

    const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);
    const auto defs = registry.definitions();

    CHECK(result.mcp_tool_count == 0);
    CHECK(result.mcp_manager == nullptr);
    CHECK(not(orangutan::testing::has_tool_named(defs, "shell")));
    CHECK(orangutan::testing::has_tool_named(defs, "read"));

    const auto shell_result = registry.execute(ToolUse("deny-shell", "shell", {{"command", "echo hello"}}));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("deny rule"));
};

TEST_CASE("FilteredToolsAreTreatedAsFilteredBeforeToolSpecificPermissionLookup") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::plan;

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("plan-filtered-shell", "shell", nlohmann::json::object()));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("Plan mode"));
    CHECK_FALSE(shell_result.content.contains("command is required"));
};

TEST_CASE("ShellApprovalAskBlocksWhenPromptUnavailable") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::default_mode;

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("ask-shell", "shell", {{"command", "echo hello"}}));
    CHECK(shell_result.is_error);
    CHECK(shell_result.content.contains("approval"));
};

TEST_CASE("ExecutionGuardHonorsRuntimeAbortChecker") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;
    auto tool_context = make_runtime_tool_context();
    tool_context.abort_checker = [] {
        return true;
    };

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));

    const auto result = registry.execute(ToolUse("aborted-shell", "shell", {{"command", "echo hello"}}));
    CHECK(result.is_error);
    CHECK(result.content == "Operation aborted by user");
};

TEST_CASE("ShellApprovalCallbackCanAllowCommand") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::default_mode;
    auto tool_context = make_runtime_tool_context();
    bool prompted = false;

    tool_context.approval_callback = [&prompted](const ToolUse &call, const PermissionDecision &decision) {
        prompted = true;
        CHECK(call.name == "shell");
        CHECK(decision.message.has_value());
        return true;
    };
    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("allow-shell", "shell", {{"command", "echo hello"}}));
    CHECK(prompted);
    CHECK(not(shell_result.is_error));
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("UsesDynamicApprovalCallbackFromToolContext") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::default_mode;
    auto tool_context = make_runtime_tool_context();
    bool prompted = false;

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));
    tool_context.approval_callback = [&prompted](const ToolUse &call, const PermissionDecision &decision) {
        prompted = true;
        CHECK(call.name == "shell");
        CHECK(decision.message.has_value());
        return true;
    };

    const auto shell_result = registry.execute(ToolUse("context-allow-shell", "shell", {{"command", "echo hello"}}));
    CHECK(prompted);
    CHECK(not(shell_result.is_error));
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("UsesUpdatedPermissionContextAfterRuntimeRegistration") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::plan;
    auto tool_context = make_runtime_tool_context();
    tool_context.permission_context = &permissions;

    static_cast<void>(register_runtime_tools(registry, nullptr, test_tmp_root(), &tool_context, {}, {}, &permissions));

    CHECK_FALSE(orangutan::testing::has_tool_named(registry.definitions(), "shell"));

    permissions.mode = permission_mode::bypass_permissions;

    CHECK(orangutan::testing::has_tool_named(registry.definitions(), "shell"));
    const auto shell_result = registry.execute(ToolUse("dynamic-shell", "shell", {{"command", "echo hello"}}));
    CHECK_FALSE(shell_result.is_error);
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("AutomationToolRunsWithoutPromptByDefault") {
    const auto automation_db = test_tmp_root() / "orangutan_automation_rule_bypass.db";
    std::filesystem::remove(automation_db);

    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::default_mode;

    automation::Repository repository(automation_db);
    automation::AutomationService service(repository);
    auto tool_context = make_runtime_tool_context();
    tool_context.automation_service = &service;

    bool prompted = false;
    tool_context.approval_callback = [&prompted](const ToolUse &, const PermissionDecision &) {
        prompted = true;
        return false;
    };

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));

    const auto result = registry.execute(ToolUse("automation-default-allow", "automation",
                                                 {
                                                     {"op", "create"},
                                                     {"name", "nightly-sync"},
                                                     {"prompt", "sync nightly"},
                                                     {"trigger", {{"type", "cron"}, {"cron", "0 * * * *"}}},
                                                 }));

    CHECK_FALSE(result.is_error);
    CHECK_FALSE(prompted);
    CHECK(result.content.contains("Created automation 'nightly-sync'"));
    std::filesystem::remove(automation_db);
};

TEST_CASE("BypassModeAllowsShellEvenWhenDenyRuleExists") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;
    permissions.deny_rules.push_back(PermissionRule{
        .source = permission_rule_source::cli_arg,
        .behavior = permission_behavior::deny,
        .tool_name = "shell",
        .content = RuleContent{.match_type = rule_match_type::prefix, .pattern = "echo"},
    });

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

    const auto shell_result = registry.execute(ToolUse("blocked-shell", "shell", {{"command", "echo hello"}}));
    CHECK_FALSE(shell_result.is_error);
    CHECK(shell_result.content.contains("hello"));
};

TEST_CASE("ScriptToolsRespectShellApprovalPolicy") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.deny_rules.push_back(PermissionRule{
        .source = permission_rule_source::cli_arg,
        .behavior = permission_behavior::deny,
        .tool_name = "echo_custom",
    });

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions));

    const auto result = registry.execute(ToolUse("deny-script-shell", "echo_custom", nlohmann::json::object()));
    CHECK(result.is_error);
    CHECK(result.content.contains("deny rule"));
};

TEST_CASE("ScriptToolsRespectDeniedShellCommands") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;
    permissions.deny_rules.push_back(PermissionRule{
        .source = permission_rule_source::cli_arg,
        .behavior = permission_behavior::deny,
        .tool_name = "echo_custom",
    });

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {}, &permissions));

    const auto result = registry.execute(ToolUse("deny-script-command", "echo_custom", nlohmann::json::object()));
    CHECK_FALSE(result.is_error);
    CHECK(result.content.contains("custom"));
};

TEST_CASE("ScriptToolsRunWithoutPromptByDefault") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::default_mode;
    auto tool_context = make_runtime_tool_context();
    bool prompted = false;

    const std::vector<Config::ScriptToolConfig> custom_tools = {{
        .name = "echo_custom",
        .description = "Echo custom",
        .command = "echo custom",
    }};

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, custom_tools, {}, &permissions));
    tool_context.approval_callback = [&prompted](const ToolUse &call, const PermissionDecision &decision) {
        prompted = true;
        CHECK(call.name == "echo_custom");
        CHECK(decision.message.has_value());
        return true;
    };

    const auto result = registry.execute(ToolUse("allow-script-shell", "echo_custom", nlohmann::json::object()));
    CHECK_FALSE(prompted);
    CHECK(not(result.is_error));
    CHECK(result.content.contains("custom"));
};

TEST_CASE("BypassPermissionsIgnoreShellAskRules") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;
    permissions.ask_rules.push_back(PermissionRule{
        .source = permission_rule_source::cli_arg,
        .behavior = permission_behavior::ask,
        .tool_name = "shell",
        .content = RuleContent{.match_type = rule_match_type::prefix, .pattern = "echo"},
    });

    auto tool_context = make_runtime_tool_context();
    bool prompted = false;
    tool_context.approval_callback = [&prompted](const ToolUse &, const PermissionDecision &) {
        prompted = true;
        return false;
    };

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, &tool_context, {}, {}, &permissions));

    const auto result = registry.execute(ToolUse("compound-shell", "shell", {{"command", "echo hello"}}));
    CHECK_FALSE(prompted);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.contains("hello"));
};

TEST_CASE("WriteToolRejectsPathsOutsidePermissionScope") {
    const auto root = test_tmp_root() / "permission-scope-block";
    const auto workspace = root / "workspace";
    const auto outside = root / "outside";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(outside);

    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;

    static_cast<void>(register_runtime_tools(registry, nullptr, workspace, nullptr, {}, {}, &permissions));

    const auto result = registry.execute(ToolUse("outside-write", "write", {{"path", (outside / "blocked.txt").string()}, {"content", "blocked"}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("workspace sandbox"));
    std::filesystem::remove_all(root);
};

TEST_CASE("WriteToolAllowsConfiguredAdditionalDirectories") {
    const auto root = test_tmp_root() / "permission-scope-allow";
    const auto workspace = root / "workspace";
    const auto outside = root / "outside";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(outside);

    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::bypass_permissions;
    permissions.additional_directories.push_back(outside.string());

    static_cast<void>(register_runtime_tools(registry, nullptr, workspace, nullptr, {}, {}, &permissions));

    const auto target = outside / "allowed.txt";
    const auto result = registry.execute(ToolUse("outside-write-allowed", "write", {{"path", target.string()}, {"content", "allowed"}}));
    CHECK_FALSE(result.is_error);
    CHECK(std::filesystem::exists(target));
    std::filesystem::remove_all(root);
};

TEST_CASE("ReadToolIsMarkedReadOnly") {
    ToolRegistry registry;
    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, nullptr));

    const auto *read_tool = registry.find_tool("read");
    REQUIRE(read_tool != nullptr);
    CHECK(read_tool->read_only);

    const auto *write_tool = registry.find_tool("write");
    REQUIRE(write_tool != nullptr);
    CHECK_FALSE(write_tool->read_only);
};

TEST_CASE("PlanModeShowsReadWriteAndEditToolsOnly") {
    ToolRegistry registry;
    ToolPermissionContext permissions;
    permissions.mode = permission_mode::plan;

    static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));
    const auto defs = registry.definitions();

    CHECK(orangutan::testing::has_tool_named(defs, "read"));
    CHECK(orangutan::testing::has_tool_named(defs, "write"));
    CHECK(orangutan::testing::has_tool_named(defs, "edit"));
    CHECK_FALSE(orangutan::testing::has_tool_named(defs, "shell"));
}

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
    const auto config_path = fixture.home() / ".orangutan" / "config.json";
    std::ofstream(config_path) << "{\n  \"agent\": {\n    \"model\": \"claude\"\n  }\n}\n";

    const auto result = fixture.registry().execute(ToolUse("cfg_read", "read", {{"path", "~/.orangutan/config.json"}}));

    CHECK(not(result.is_error));
    CHECK(result.content.contains("\"model\": \"claude\""));
};

TEST_CASE("WriteAllowsOrangutanConfigOutsideWorkspace") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto config_path = fixture.home() / ".orangutan" / "config.json";

    const auto result =
        fixture.registry().execute(ToolUse("cfg_write", "write", {{"path", "~/.orangutan/config.json"}, {"content", "{\n  \"agent\": {\n    \"model\": \"gpt\"\n  }\n}\n"}}));

    CHECK(not(result.is_error));

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "{\n  \"agent\": {\n    \"model\": \"gpt\"\n  }\n}\n");
};

TEST_CASE("EditAllowsOrangutanConfigOutsideWorkspace") {
    BuiltinToolsWorkspaceConfigAccessTest fixture;
    const auto config_path = fixture.home() / ".orangutan" / "config.json";
    std::ofstream(config_path) << "{\n  \"agent\": {\n    \"model\": \"claude\"\n  }\n}\n";

    const std::string patch = "*** ~/.orangutan/config.json\n"
                              "<<<<<<< SEARCH\n"
                              "\"model\": \"claude\"\n"
                              "=======\n"
                              "\"model\": \"gpt\"\n"
                              ">>>>>>> REPLACE\n";

    const auto result = fixture.registry().execute(ToolUse("cfg_edit", "edit", {{"patch", patch}}));

    CHECK(not(result.is_error));

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "{\n  \"agent\": {\n    \"model\": \"gpt\"\n  }\n}\n");
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
        // Explicitly pass hashline — the default is search_replace
        register_builtin_tools(registry_, nullptr, workspace_.string(), nullptr, nullptr, tools::file::edit_mode::hashline);
    }

    ~HashlineToolsTest() {
        tmp_env_.reset();
        std::filesystem::remove_all(workspace_);
    }
    HashlineToolsTest(const HashlineToolsTest &) = delete;
    HashlineToolsTest &operator=(const HashlineToolsTest &) = delete;
    HashlineToolsTest(HashlineToolsTest &&) = delete;
    HashlineToolsTest &operator=(HashlineToolsTest &&) = delete;

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

    auto hash = orangutan::tools::compute_line_hash("    return 0;", 2);
    std::string anchor = "2#" + hash;

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"anchor", anchor}, {"content", nlohmann::json::array({"    return 42;"})}}});

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

    auto hash = orangutan::tools::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", nlohmann::json::array({"ccc"})}}});

    const auto result = fixture.registry().execute(ToolUse("e1b", "edit", {{"path", "noeol.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "noeol.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nccc");
};

TEST_CASE("EditDeleteLine") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "del.txt") << "aaa\nbbb\nccc\n";

    auto hash = orangutan::tools::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "delete"}, {"anchor", "2#" + hash}}});

    const auto result = fixture.registry().execute(ToolUse("e2", "edit", {{"path", "del.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "del.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nccc\n");
};

TEST_CASE("EditInsertAfterEOF") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "ins.txt") << "aaa\nbbb\n";

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "insert_after"}, {"content", nlohmann::json::array({"ccc"})}}});

    const auto result = fixture.registry().execute(ToolUse("e3", "edit", {{"path", "ins.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "ins.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nbbb\nccc\n");
};

TEST_CASE("EditHashMismatchReturnsErrorWithContext") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "stale.txt") << "aaa\nbbb\nccc\n";

    auto actual_hash = orangutan::tools::compute_line_hash("bbb", 2);
    const std::string wrong_hash = actual_hash == "ZZ" ? "ZY" : "ZZ";
    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"anchor", "2#" + wrong_hash}, {"content", nlohmann::json::array({"XXX"})}}});

    const auto result = fixture.registry().execute(ToolUse("e4", "edit", {{"path", "stale.txt"}, {"edits", edits}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("mismatch"));
    CHECK(result.content.contains(actual_hash));
};

TEST_CASE("EditContentAsStringIsSplitOnNewlines") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "str.txt") << "aaa\nbbb\n";

    auto hash = orangutan::tools::compute_line_hash("bbb", 2);
    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", "line1\nline2"}}});

    const auto result = fixture.registry().execute(ToolUse("e5", "edit", {{"path", "str.txt"}, {"edits", edits}}));
    CHECK(not(result.is_error));

    std::ifstream ifs(fixture.workspace() / "str.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(content == "aaa\nline1\nline2\n");
};

TEST_CASE("EditMissingAnchorForReplaceReturnsError") {
    HashlineToolsTest fixture;
    std::ofstream(fixture.workspace() / "missing.txt") << "aaa\n";

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"content", nlohmann::json::array({"XXX"})}}});

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
    register_builtin_tools(sr_registry, nullptr, sr_workspace.string(), nullptr, nullptr, tools::file::edit_mode::search_replace);

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

// ── Deferred tool loading ──────────────────────────

TEST_CASE("DeferredToolsExcludedFromDefinitions") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "core", .description = "Always loaded"}, .execute = [](const nlohmann::json &) {
                                return "ok";
                            }});
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    const auto defs = registry.definitions();
    CHECK(defs.size() == 1);
    CHECK(defs[0].name == "core");
};

TEST_CASE("DiscoveredDeferredToolIncludedInDefinitions") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "core", .description = "Always loaded"}, .execute = [](const nlohmann::json &) {
                                return "ok";
                            }});
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    registry.discover_tool("mcp_foo");

    const auto defs = registry.definitions();
    CHECK(defs.size() == 2);
    CHECK(orangutan::testing::has_tool_named(defs, "core"));
    CHECK(orangutan::testing::has_tool_named(defs, "mcp_foo"));
};

TEST_CASE("DeferredToolStillExecutable") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "mcp result";
                                },
                            .deferred = true});

    const auto result = registry.execute(ToolUse("id_mcp", "mcp_foo", {}));
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "mcp result");
};

TEST_CASE("DeferredToolSummariesListsUndiscoveredOnly") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "mcp_a", .description = "Tool A"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});
    registry.register_tool({.definition = {.name = "mcp_b", .description = "Tool B"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    registry.discover_tool("mcp_a");

    const auto summaries = registry.deferred_tool_summaries();
    CHECK(summaries.size() == 1);
    CHECK(summaries[0].name == "mcp_b");
};

TEST_CASE("HasDeferredToolsReturnsFalseWhenNone") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "core", .description = "Always loaded"}, .execute = [](const nlohmann::json &) {
                                return "ok";
                            }});

    CHECK_FALSE(registry.has_deferred_tools());
};

TEST_CASE("HasDeferredToolsReturnsTrueWhenPresent") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    CHECK(registry.has_deferred_tools());
};

TEST_CASE("ToolSearchFindsMcpToolsFromProcessRegistryForMultiWordQueries") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "github:list_repositories", .description = "List repositories from GitHub"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});
    register_tool_search(registry);

    const auto result = registry.execute(ToolUse("tool-search-mcp", "tool_search", {{"query", "mcp github repositories"}}));

    CHECK_FALSE(result.is_error);
    CHECK(result.content.contains("github:list_repositories"));
    CHECK(result.content.contains("select:"));
};

TEST_CASE("ClearDiscoveredResetsDeferredTools") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool"},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    registry.discover_tool("mcp_foo");
    CHECK(registry.definitions().size() == 1);

    registry.clear_discovered();
    CHECK(registry.definitions().empty());
};

TEST_CASE("FindDefinitionReturnsDeferredTool") {
    ToolRegistry registry;
    registry.register_tool({.definition = {.name = "mcp_foo", .description = "An MCP tool", .input_schema = {{"type", "object"}}},
                            .execute =
                                [](const nlohmann::json &) {
                                    return "ok";
                                },
                            .deferred = true});

    const auto *def = registry.find_definition("mcp_foo");
    REQUIRE(def != nullptr);
    CHECK(def->name == "mcp_foo");
    CHECK(def->description == "An MCP tool");

    CHECK(registry.find_definition("nonexistent") == nullptr);
};

TEST_CASE("SkillToolBlocksManualOnlySkillsForAutomaticOrigin") {
    const auto skill_root = test_tmp_root() / "skill-tool-manual-only";
    std::filesystem::remove_all(skill_root);
    write_skill_file(skill_root, "manual-only", "name: manual-only\ndescription: manual only skill\nscope: manual_only", "manual body");

    SkillLoader loader;
    loader.load_from_directories({skill_root});

    ToolRegistry registry;
    register_skill_tool(registry, loader);

    const auto result = registry.execute(ToolUse("skill-manual-only", "skill", {{"name", "manual-only"}}));
    CHECK_FALSE(result.is_error);
    CHECK(result.content.contains("cannot be auto-invoked"));

    std::filesystem::remove_all(skill_root);
};

TEST_CASE("SkillToolBlocksInactiveConditionalSkillsForAutomaticOrigin") {
    const auto skill_root = test_tmp_root() / "skill-tool-inactive-conditional";
    std::filesystem::remove_all(skill_root);
    write_skill_file(skill_root, "conditional", "name: conditional\ndescription: conditional skill\nscope: conditional\npaths_any: [src/*.cpp]", "conditional body");

    SkillLoader loader;
    const auto workspace = test_tmp_root() / "skill-tool-inactive-conditional-workspace";
    loader.set_workspace_root(workspace);
    loader.load_from_directories({skill_root});

    ToolRegistry registry;
    register_skill_tool(registry, loader);

    const auto result = registry.execute(ToolUse("skill-conditional", "skill", {{"name", "conditional"}}));
    CHECK_FALSE(result.is_error);
    CHECK(result.content.contains("inactive for current context"));

    std::filesystem::remove_all(skill_root);
    std::filesystem::remove_all(workspace);
};
