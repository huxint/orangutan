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

#include "support/ut.hpp"
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

using namespace boost::ut;

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
    expect(not(result.is_error));
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
        expect(not(result.is_error));
        if (result.is_error) {
            return {};
        }

        last_snapshot = json::parse(result.content);
        if (!last_snapshot.value("running", true)) {
            return last_snapshot;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    expect(false) << "background process did not finish in time: " << process_id;
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

boost::ut::suite tool_registry_test_starts_empty_suite = [] {
    "StartsEmpty"_test = [] {
        const ToolRegistry registry;
        expect(registry.definitions().empty());
    };
};

boost::ut::suite tool_registry_test_register_and_retrieve_definition_suite = [] {
    "RegisterAndRetrieveDefinition"_test = [] {
        ToolRegistry registry;
        registry.register_tool({.definition = {.name = "echo", .description = "Echoes input", .input_schema = {{"type", "object"}}}, .execute = [](const json &input) {
                                    return input.at("text").get<std::string>();
                                }});

        const auto defs = registry.definitions();
        expect(((defs.size()) == (1)) >> fatal);
        expect(defs[0].name == "echo");
        expect(defs[0].description == "Echoes input");
    };
};

boost::ut::suite tool_registry_test_executes_registered_tool_suite = [] {
    "ExecutesRegisteredTool"_test = [] {
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

        expect(result.tool_use_id == "id_1");
        expect(result.content == "Hello, Alice!");
        expect(not(result.is_error));
    };
};

boost::ut::suite tool_registry_test_unknown_tool_returns_error_suite = [] {
    "UnknownToolReturnsError"_test = [] {
        const ToolRegistry registry;
        const ToolUseBlock call{
            .id = "id_2",
            .name = "nonexistent",
            .input = {},
        };
        const auto result = registry.execute(call);

        expect(result.tool_use_id == "id_2");
        expect(result.is_error);
    };
};

boost::ut::suite tool_registry_test_tool_exception_becomes_error_suite = [] {
    "ToolExceptionBecomesError"_test = [] {
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

        expect(result.is_error);
        expect(result.content.find("kaboom") != std::string::npos);
    };
};

boost::ut::suite tool_registry_test_duplicate_registration_replaces_existing_executor_suite = [] {
    "DuplicateRegistrationReplacesExistingExecutor"_test = [] {
        ToolRegistry registry;
        registry.register_tool({.definition = {.name = "echo", .description = "First"}, .execute = [](const json &) {
                                    return std::string{"first"};
                                }});
        registry.register_tool({.definition = {.name = "echo", .description = "Second"}, .execute = [](const json &) {
                                    return std::string{"second"};
                                }});

        const auto defs = registry.definitions();
        expect(((defs.size()) == (1)) >> fatal);
        expect(defs[0].description == "Second");

        const auto result = registry.execute(ToolUseBlock{
            .id = "id_dup",
            .name = "echo",
            .input = {},
        });
        expect(not(result.is_error));
        expect(result.content == "second");
    };
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

boost::ut::suite builtin_tools_test_registers_expected_core_tools_suite = [] {
    "RegistersExpectedCoreTools"_test = [] {
        BuiltinToolsTest fixture;
        const auto defs = fixture.registry().definitions();
        expect(defs.size() == 7);
        expect(has_tool_named(defs, "shell"));
        expect(has_tool_named(defs, "process_list"));
        expect(has_tool_named(defs, "process_poll"));
        expect(has_tool_named(defs, "process_kill"));
        expect(has_tool_named(defs, "read"));
        expect(has_tool_named(defs, "write"));
        expect(has_tool_named(defs, "edit"));
    };
};

boost::ut::suite builtin_tools_test_does_not_register_subagent_tools_without_runtime_context_suite = [] {
    "DoesNotRegisterSubagentToolsWithoutRuntimeContext"_test = [] {
        BuiltinToolsTest fixture;
        const auto defs = fixture.registry().definitions();

        for (const auto &def : defs) {
            expect(def.name != "subagent_spawn");
            expect(def.name != "subagent_status");
            expect(def.name != "subagent_wait");
        }
    };
};

boost::ut::suite builtin_tools_test_does_not_register_memory_tools_without_memory_store_suite = [] {
    "DoesNotRegisterMemoryToolsWithoutMemoryStore"_test = [] {
        BuiltinToolsTest fixture;
        const auto defs = fixture.registry().definitions();

        expect(not(has_tool_named(defs, "remember")));
        expect(not(has_tool_named(defs, "recall")));
        expect(not(has_tool_named(defs, "forget")));
        expect(not(has_tool_named(defs, "memory_store")));
        expect(not(has_tool_named(defs, "memory_recall")));
        expect(not(has_tool_named(defs, "memory_forget")));
        expect(not(has_tool_named(defs, "memory_update")));
        expect(not(has_tool_named(defs, "memory_list")));
        expect(not(has_tool_named(defs, "memory_stats")));
    };
};

boost::ut::suite builtin_tools_test_shell_runs_simple_command_suite = [] {
    "ShellRunsSimpleCommand"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_sh",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("hello") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_test_shell_reports_non_zero_exit_code_suite = [] {
    "ShellReportsNonZeroExitCode"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_sh2",
            .name = "shell",
            .input = {{"command", "false"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("exit code") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_test_read_file_returns_line_numbered_contents_suite = [] {
    "ReadFileReturnsLineNumberedContents"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        // cat -n format: right-aligned 6-char field, tab, content
        expect(result.content.find("1\taaa\n") != std::string::npos);
        expect(result.content.find("2\tbbb\n") != std::string::npos);
        expect(result.content.find("3\tccc\n") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_file_missing_returns_error_suite = [] {
    "ReadFileMissingReturnsError"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_rf2",
            .name = "read",
            .input = {{"path", "/tmp/orangutan_nonexistent_file_xyz.txt"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
        expect(result.content.find("not found") != std::string::npos);
    };
};

// ── write tool ──────────────────────────────────

boost::ut::suite builtin_tools_test_write_creates_file_suite = [] {
    "WriteCreatesFile"_test = [] {
        BuiltinToolsTest fixture;
        const auto tmp = test_tmp_root() / "orangutan_write_test.txt";
        std::filesystem::remove(tmp); // ensure clean state

        const ToolUseBlock call{
            .id = "id_wf",
            .name = "write",
            .input = {{"path", tmp.string()}, {"content", "hello world"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("11 bytes") != std::string::npos);

        // Verify file contents
        std::ifstream ifs(tmp);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "hello world");

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_write_creates_parent_directories_suite = [] {
    "WriteCreatesParentDirectories"_test = [] {
        BuiltinToolsTest fixture;
        const auto tmp = test_tmp_root() / "orangutan_test_dir" / "sub" / "file.txt";
        std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");

        const ToolUseBlock call{
            .id = "id_wf2",
            .name = "write",
            .input = {{"path", tmp.string()}, {"content", "nested"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(std::filesystem::exists(tmp));

        std::filesystem::remove_all(test_tmp_root() / "orangutan_test_dir");
    };
};

// ── read tool — offset/limit ────────────────────

boost::ut::suite builtin_tools_test_read_file_with_offset_suite = [] {
    "ReadFileWithOffset"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("10\tline10") != std::string::npos);
        expect(result.content.find("20\tline20") != std::string::npos);
        // Should NOT contain lines before offset
        expect(result.content.find("\tline9\n") == std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_file_with_limit_suite = [] {
    "ReadFileWithLimit"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("1\tline1") != std::string::npos);
        expect(result.content.find("5\tline5") != std::string::npos);
        // Should have truncation summary
        expect(result.content.find("showing 5 of 100 lines") != std::string::npos);
        // Should NOT contain line 6
        expect(result.content.find("\tline6\n") == std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_file_with_offset_and_limit_suite = [] {
    "ReadFileWithOffsetAndLimit"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("100\tline100") != std::string::npos);
        expect(result.content.find("149\tline149") != std::string::npos);
        expect(result.content.find("showing 50 of 500 lines") != std::string::npos);
        expect(result.content.find("\tline99\n") == std::string::npos);
        expect(result.content.find("\tline150\n") == std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_file_offset_beyond_eof_suite = [] {
    "ReadFileOffsetBeyondEOF"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("No content at offset 100") != std::string::npos);
        expect(result.content.find("file has 3 lines") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_rejects_non_positive_offset_suite = [] {
    "ReadRejectsNonPositiveOffset"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
        expect(result.content.find("offset") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_rejects_non_positive_limit_suite = [] {
    "ReadRejectsNonPositiveLimit"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
        expect(result.content.find("limit") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

// ── read tool — binary detection ────────────────

boost::ut::suite builtin_tools_test_read_binary_file_returns_metadata_suite = [] {
    "ReadBinaryFileReturnsMetadata"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("Binary file:") != std::string::npos);
        expect(result.content.find(".bin") != std::string::npos);
        expect(result.content.find("bytes") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_text_file_reads_normally_suite = [] {
    "ReadTextFileReadsNormally"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("Binary file:") == std::string::npos);
        expect(result.content.find("just text") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

// ── read tool — multi-path ──────────────────────

boost::ut::suite builtin_tools_test_read_multiple_files_suite = [] {
    "ReadMultipleFiles"_test = [] {
        BuiltinToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("=== " + tmp_a.string() + " ===") != std::string::npos);
        expect(result.content.find("=== " + tmp_b.string() + " ===") != std::string::npos);
        expect(result.content.find("content_a") != std::string::npos);
        expect(result.content.find("content_b") != std::string::npos);

        std::filesystem::remove(tmp_a);
        std::filesystem::remove(tmp_b);
    };
};

boost::ut::suite builtin_tools_test_read_multi_path_one_fails_continues_others_suite = [] {
    "ReadMultiPathOneFailsContinuesOthers"_test = [] {
        BuiltinToolsTest fixture;
        const auto tmp = test_tmp_root() / "orangutan_multi_ok.txt";
        {
            std::ofstream(tmp) << "good content\n";
        }

        const ToolUseBlock call{
            .id = "id_mpf",
            .name = "read",
            .input = {{"paths", json::array({tmp.string(), "/tmp/orangutan_nonexistent_xyz.txt"})}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("good content") != std::string::npos);
        expect(result.content.find("Error:") != std::string::npos);

        std::filesystem::remove(tmp);
    };
};

boost::ut::suite builtin_tools_test_read_both_path_and_paths_returns_error_suite = [] {
    "ReadBothPathAndPathsReturnsError"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_both",
            .name = "read",
            .input = {{"path", "/tmp/a.txt"}, {"paths", json::array({"/tmp/b.txt"})}},
        };
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
        expect(result.content.find("not both") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_test_read_neither_path_nor_paths_returns_error_suite = [] {
    "ReadNeitherPathNorPathsReturnsError"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_neither",
            .name = "read",
            .input = json::object(),
        };
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
        expect(result.content.find("Required") != std::string::npos);
    };
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

boost::ut::suite runtime_tool_loader_test_registers_builtin_and_custom_tools_only_suite = [] {
    "RegistersBuiltinAndCustomToolsOnly"_test = [] {
        ToolRegistry registry;
        const std::vector<Config::ScriptToolConfig> custom_tools = {{
            .name = "echo_custom",
            .description = "Echo custom",
            .command = "printf custom",
        }};

        const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, custom_tools, {});
        const auto defs = registry.definitions();

        expect(result.mcp_tool_count == 0);
        expect(result.mcp_manager == nullptr);
        expect(has_tool_named(defs, "shell"));
        expect(has_tool_named(defs, "process_list"));
        expect(has_tool_named(defs, "process_poll"));
        expect(has_tool_named(defs, "process_kill"));
        expect(has_tool_named(defs, "read"));
        expect(has_tool_named(defs, "write"));
        expect(has_tool_named(defs, "edit"));
        expect(not(has_tool_named(defs, "ls")));
        expect(not(has_tool_named(defs, "grep")));
        expect(has_tool_named(defs, "echo_custom"));
    };
};

boost::ut::suite runtime_tool_loader_test_registers_usable_memory_and_subagent_tools_together_suite = [] {
    "RegistersUsableMemoryAndSubagentToolsTogether"_test = [] {
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
            const auto parent_session_id = session_store.create_empty("test-model", "scope:parent");
            const auto child_session_id = session_store.create_empty("test-model", "scope:child");

            {
                SubagentRunStore run_store(session_db);
                SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
                    return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
                });
                auto current_session_id = parent_session_id;
                const auto tool_context = make_runtime_tool_context(&manager, &current_session_id);

                const auto result = register_runtime_tools(registry, &runtime_memory, {}, &tool_context, {}, {});
                const auto defs = registry.definitions();

                expect(result.mcp_tool_count == 0);
                expect(result.mcp_manager == nullptr);
                expect(has_tool_named(defs, "shell"));
                expect(has_tool_named(defs, "process_list"));
                expect(has_tool_named(defs, "process_poll"));
                expect(has_tool_named(defs, "process_kill"));
                expect(has_tool_named(defs, "read"));
                expect(not(has_tool_named(defs, "ls")));
                expect(not(has_tool_named(defs, "grep")));
                expect(has_tool_named(defs, "subagent_spawn"));
                expect(has_tool_named(defs, "subagent_status"));
                expect(has_tool_named(defs, "subagent_wait"));
                expect(has_tool_named(defs, "remember"));
                expect(has_tool_named(defs, "memory_recall"));
                expect(has_tool_named(defs, "memory_stats"));

                const auto remember = registry.execute(ToolUseBlock{
                    .id = "remember-runtime",
                    .name = "remember",
                    .input = {{"key", "theme"}, {"content", "blue"}, {"category", "prefs"}},
                });
                expect(not(remember.is_error));

                const auto recall = registry.execute(ToolUseBlock{
                    .id = "recall-runtime",
                    .name = "memory_recall",
                    .input = {{"query", "theme"}},
                });
                expect(not(recall.is_error));
                expect(recall.content.find("blue") != std::string::npos);

                const auto spawn = registry.execute(ToolUseBlock{
                    .id = "spawn-runtime",
                    .name = "subagent_spawn",
                    .input = {{"child_agent_key", "reviewer"},
                              {"child_scope_key", "scope:child"},
                              {"child_session_id", child_session_id},
                              {"task_summary", "Review the tool layout refactor"}},
                });
                expect(not(spawn.is_error));
                const auto spawn_payload = json::parse(spawn.content);
                expect((spawn_payload.at("accepted").get<bool>()) >> fatal);
                const auto run_id = spawn_payload.at("run_id").get<std::string>();
                expect(not(run_id.empty()));

                const auto wait = registry.execute(ToolUseBlock{
                    .id = "wait-runtime",
                    .name = "subagent_wait",
                    .input = {{"run_id", run_id}, {"timeout_ms", 1000}},
                });
                expect(not(wait.is_error));
                const auto wait_payload = json::parse(wait.content);
                expect(wait_payload.at("state").get<std::string>() == "completed");
            }
        }

        std::filesystem::remove(memory_db);
        std::filesystem::remove(session_db);
    };
};

boost::ut::suite runtime_tool_loader_test_denied_tools_are_hidden_and_blocked_by_policy_suite = [] {
    "DeniedToolsAreHiddenAndBlockedByPolicy"_test = [] {
        ToolRegistry registry;
        ToolPermissionSettings permissions;
        permissions.denied_tools = {"shell"};

        const auto result = register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions);
        const auto defs = registry.definitions();

        expect(result.mcp_tool_count == 0);
        expect(result.mcp_manager == nullptr);
        expect(not(has_tool_named(defs, "shell")));
        expect(has_tool_named(defs, "read"));

        const auto shell_result = registry.execute(ToolUseBlock{
            .id = "deny-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(shell_result.is_error);
        expect(shell_result.content.find("permission policy") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_shell_approval_ask_blocks_when_prompt_unavailable_suite = [] {
    "ShellApprovalAskBlocksWhenPromptUnavailable"_test = [] {
        ToolRegistry registry;
        ToolPermissionSettings permissions;
        permissions.sandbox_mode = ToolSandboxMode::disabled;
        permissions.shell_approval = ToolApprovalPolicy::ask;

        static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

        const auto shell_result = registry.execute(ToolUseBlock{
            .id = "ask-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(shell_result.is_error);
        expect(shell_result.content.find("requires approval") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_shell_approval_callback_can_allow_command_suite = [] {
    "ShellApprovalCallbackCanAllowCommand"_test = [] {
        ToolRegistry registry;
        ToolPermissionSettings permissions;
        permissions.sandbox_mode = ToolSandboxMode::disabled;
        permissions.shell_approval = ToolApprovalPolicy::ask;

        bool prompted = false;
        static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions, [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
            prompted = true;
            expect(call.name == "shell");
            expect(prompt_text.find("echo hello") != std::string::npos);
            return true;
        }));

        const auto shell_result = registry.execute(ToolUseBlock{
            .id = "allow-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(prompted);
        expect(not(shell_result.is_error));
        expect(shell_result.content.find("hello") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_uses_dynamic_approval_callback_from_tool_context_suite = [] {
    "UsesDynamicApprovalCallbackFromToolContext"_test = [] {
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
        tool_context.approval_callback = [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
            prompted = true;
            expect(call.name == "shell");
            expect(prompt_text.find("echo hello") != std::string::npos);
            return true;
        };

        const auto shell_result = registry.execute(ToolUseBlock{
            .id = "context-allow-shell",
            .name = "shell",
            .input = {{"command", "echo hello"}},
        });
        expect(prompted);
        expect(not(shell_result.is_error));
        expect(shell_result.content.find("hello") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_blocked_shell_commands_are_rejected_by_policy_suite = [] {
    "BlockedShellCommandsAreRejectedByPolicy"_test = [] {
        ToolRegistry registry;
        ToolPermissionSettings permissions;
        permissions.sandbox_mode = ToolSandboxMode::disabled;
        permissions.shell_approval = ToolApprovalPolicy::allow;
        permissions.denied_shell_commands = {"rm -rf", "shutdown"};

        static_cast<void>(register_runtime_tools(registry, nullptr, {}, nullptr, {}, {}, &permissions));

        const auto shell_result = registry.execute(ToolUseBlock{
            .id = "blocked-shell",
            .name = "shell",
            .input = {{"command", "rm -rf build"}},
        });
        expect(shell_result.is_error);
        expect(shell_result.content.find("matched 'rm -rf'") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_script_tools_respect_shell_approval_policy_suite = [] {
    "ScriptToolsRespectShellApprovalPolicy"_test = [] {
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

        const auto result = registry.execute(ToolUseBlock{
            .id = "deny-script-shell",
            .name = "echo_custom",
            .input = json::object(),
        });
        expect(result.is_error);
        expect(result.content.find("approval policy") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_script_tools_respect_denied_shell_commands_suite = [] {
    "ScriptToolsRespectDeniedShellCommands"_test = [] {
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

        const auto result = registry.execute(ToolUseBlock{
            .id = "deny-script-command",
            .name = "wipe",
            .input = {{"path", "build"}},
        });
        expect(result.is_error);
        expect(result.content.find("matched 'rm -rf'") != std::string::npos);
    };
};

boost::ut::suite runtime_tool_loader_test_script_tools_use_dynamic_approval_callback_from_tool_context_suite = [] {
    "ScriptToolsUseDynamicApprovalCallbackFromToolContext"_test = [] {
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
        tool_context.approval_callback = [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
            prompted = true;
            expect(call.name == "echo_custom");
            expect(prompt_text.find("echo custom") != std::string::npos);
            return true;
        };

        const auto result = registry.execute(ToolUseBlock{
            .id = "allow-script-shell",
            .name = "echo_custom",
            .input = json::object(),
        });
        expect(prompted);
        expect(not(result.is_error));
        expect(result.content.find("custom") != std::string::npos);
    };
};

boost::ut::suite script_tools_test_ls_lists_directory_suite = [] {
    "LsListsDirectory"_test = [] {
        ScriptToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("subdir") != std::string::npos);
        expect(result.content.find("file.txt") != std::string::npos);

        std::filesystem::remove_all(tmp_dir);
    };
};

boost::ut::suite script_tools_test_ls_missing_path_returns_error_suite = [] {
    "LsMissingPathReturnsError"_test = [] {
        ScriptToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_ls2",
            .name = "ls",
            .input = {{"path", "/tmp/orangutan_nonexistent_dir_xyz"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(result.is_error);
    };
};

// ── grep tool (default script tool) ─────────────

boost::ut::suite script_tools_test_grep_finds_pattern_suite = [] {
    "GrepFindsPattern"_test = [] {
        ScriptToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("hello world") != std::string::npos);
        expect(result.content.find("a.txt") != std::string::npos);

        std::filesystem::remove_all(tmp_dir);
    };
};

boost::ut::suite script_tools_test_grep_no_matches_returns_error_suite = [] {
    "GrepNoMatchesReturnsError"_test = [] {
        ScriptToolsTest fixture;
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
        const auto result = fixture.registry().execute(call);

        // rg returns exit code 1 for no matches, which becomes an error in script tool
        expect(result.is_error);

        std::filesystem::remove_all(tmp_dir);
    };
};

// ── credential scrubbing ────────────────────────

boost::ut::suite builtin_tools_test_scrubs_api_key_in_shell_output_suite = [] {
    "ScrubsApiKeyInShellOutput"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_scrub",
            .name = "shell",
            .input = {{"command", "echo 'api_key: sk-ant-api03-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("[REDACTED]") != std::string::npos);
        expect(result.content.find("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") == std::string::npos);
    };
};

boost::ut::suite builtin_tools_test_scrubs_bearer_token_suite = [] {
    "ScrubsBearerToken"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_scrub2",
            .name = "shell",
            .input = {{"command", "echo 'Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.xxxxxxxxxxxxx'"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("[REDACTED]") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_test_does_not_scrub_normal_output_suite = [] {
    "DoesNotScrubNormalOutput"_test = [] {
        BuiltinToolsTest fixture;
        const ToolUseBlock call{
            .id = "id_noscrub",
            .name = "shell",
            .input = {{"command", "echo 'hello world this is normal output'"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find("[REDACTED]") == std::string::npos);
        expect(result.content.find("hello world") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_relative_paths_resolve_against_workspace_suite = [] {
    "RelativePathsResolveAgainstWorkspace"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const ToolUseBlock write_call{
            .id = "id_ws_write",
            .name = "write",
            .input = {{"path", "notes/todo.txt"}, {"content", "ship it"}},
        };
        const auto write_result = fixture.registry().execute(write_call);

        expect(not(write_result.is_error));
        expect(std::filesystem::exists(fixture.workspace() / "notes" / "todo.txt"));

        const ToolUseBlock read_call{
            .id = "id_ws_read",
            .name = "read",
            .input = {{"path", "notes/todo.txt"}},
        };
        const auto read_result = fixture.registry().execute(read_call);

        expect(not(read_result.is_error));
        expect(read_result.content.find("ship it") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_absolute_paths_inside_workspace_remain_allowed_suite = [] {
    "AbsolutePathsInsideWorkspaceRemainAllowed"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto file_path = fixture.workspace() / "notes" / "absolute.txt";
        std::filesystem::create_directories(file_path.parent_path());
        std::ofstream(file_path) << "inside sandbox\n";

        const ToolUseBlock read_call{
            .id = "id_ws_abs_read",
            .name = "read",
            .input = {{"path", file_path.string()}},
        };
        const auto read_result = fixture.registry().execute(read_call);

        expect(not(read_result.is_error));
        expect(read_result.content.find("inside sandbox") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_read_rejects_absolute_path_outside_workspace_suite = [] {
    "ReadRejectsAbsolutePathOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_read.txt";
        std::ofstream(outside_path) << "outside sandbox\n";

        const ToolUseBlock read_call{
            .id = "id_ws_escape_read",
            .name = "read",
            .input = {{"path", outside_path.string()}},
        };
        const auto read_result = fixture.registry().execute(read_call);

        expect(read_result.is_error);
        expect(read_result.content.find("workspace sandbox") != std::string::npos);

        std::filesystem::remove(outside_path);
    };
};

boost::ut::suite builtin_tools_workspace_test_write_rejects_traversal_outside_workspace_suite = [] {
    "WriteRejectsTraversalOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_write.txt";
        std::filesystem::remove(outside_path);

        const ToolUseBlock write_call{
            .id = "id_ws_escape_write",
            .name = "write",
            .input = {{"path", "../orangutan_escape_write.txt"}, {"content", "escaped"}},
        };
        const auto write_result = fixture.registry().execute(write_call);

        expect(write_result.is_error);
        expect(write_result.content.find("workspace sandbox") != std::string::npos);
        expect(not(std::filesystem::exists(outside_path)));
    };
};

boost::ut::suite builtin_tools_workspace_test_shell_runs_inside_workspace_suite = [] {
    "ShellRunsInsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const ToolUseBlock call{
            .id = "id_ws_shell",
            .name = "shell",
            .input = {{"command", "pwd"}},
        };
        const auto result = fixture.registry().execute(call);

        expect(not(result.is_error));
        expect(result.content.find(fixture.workspace().string()) != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_shell_background_returns_process_and_polls_to_completion_suite = [] {
    "ShellBackgroundReturnsProcessAndPollsToCompletion"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        std::filesystem::create_directories(fixture.workspace() / "ops");

        const auto start_payload = start_background_process(fixture.registry(), "printf 'tick\\n'; sleep 0.2; pwd; printf 'tock\\n'", "ops");
        const auto process_id = start_payload.at("process_id").get<std::string>();

        expect(not(process_id.empty()));
        expect(start_payload.at("running").get<bool>());

        const auto snapshot = wait_for_background_process(fixture.registry(), process_id);
        expect(snapshot.at("status").get<std::string>() == "completed");
        expect(snapshot.at("exit_code").get<int>() == 0);
        expect(snapshot.at("stdout").get<std::string>().find("tick") != std::string::npos);
        expect(snapshot.at("stdout").get<std::string>().find((fixture.workspace() / "ops").string()) != std::string::npos);
        expect(snapshot.at("stdout").get<std::string>().find("tock") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_process_list_includes_running_background_process_suite = [] {
    "ProcessListIncludesRunningBackgroundProcess"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto start_payload = start_background_process(fixture.registry(), "python3 -c \"import time; print('ready', flush=True); time.sleep(10)\"");
        const auto process_id = start_payload.at("process_id").get<std::string>();

        const auto list_result = fixture.registry().execute(ToolUseBlock{
            .id = "list-background",
            .name = "process_list",
            .input = json::object(),
        });
        expect((not(list_result.is_error)) >> fatal);

        const auto list_payload = json::parse(list_result.content);
        expect((list_payload.contains("processes")) >> fatal);
        const auto &processes = list_payload.at("processes");
        const auto it = std::ranges::find_if(processes, [&](const json &process) {
            return process.at("process_id").get<std::string>() == process_id;
        });
        expect(((it) != (processes.end())) >> fatal);
        expect(it->at("running").get<bool>());

        const auto kill_result = fixture.registry().execute(ToolUseBlock{
            .id = "kill-after-list",
            .name = "process_kill",
            .input = {{"process_id", process_id}},
        });
        expect(not(kill_result.is_error));
    };
};

boost::ut::suite builtin_tools_workspace_test_process_kill_stops_background_process_suite = [] {
    "ProcessKillStopsBackgroundProcess"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto start_payload = start_background_process(fixture.registry(), "python3 -c \"import time; time.sleep(10)\"");
        const auto process_id = start_payload.at("process_id").get<std::string>();

        const auto kill_result = fixture.registry().execute(ToolUseBlock{
            .id = "kill-background",
            .name = "process_kill",
            .input = {{"process_id", process_id}},
        });
        expect((not(kill_result.is_error)) >> fatal);

        const auto kill_payload = json::parse(kill_result.content);
        expect(not(kill_payload.at("running").get<bool>()));
        expect(kill_payload.at("kill_requested").get<bool>());
        expect(kill_payload.at("status").get<std::string>() != "running");
        expect(not(kill_payload.at("signal_number").is_null()));
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_rejects_paths_outside_workspace_suite = [] {
    "ApplyPatchRejectsPathsOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto outside_path = fixture.workspace().parent_path() / "orangutan_escape_patch.cpp";
        std::ofstream(outside_path) << "outside\n";

        const std::string patch = "*** ../orangutan_escape_patch.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "outside\n"
                                  "=======\n"
                                  "still outside\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_escape", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("workspace sandbox") != std::string::npos);

        std::ifstream ifs(outside_path);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "outside\n");

        std::filesystem::remove(outside_path);
    };
};

boost::ut::suite builtin_tools_workspace_config_access_test_read_allows_orangutan_config_outside_workspace_suite = [] {
    "ReadAllowsOrangutanConfigOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceConfigAccessTest fixture;
        const auto config_path = fixture.home() / ".orangutan" / "config.toml";
        std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

        const auto result = fixture.registry().execute({
            .id = "cfg_read",
            .name = "read",
            .input = {{"path", "~/.orangutan/config.toml"}},
        });

        expect(not(result.is_error));
        expect(result.content.find("model = \"claude\"") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_config_access_test_write_allows_orangutan_config_outside_workspace_suite = [] {
    "WriteAllowsOrangutanConfigOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceConfigAccessTest fixture;
        const auto config_path = fixture.home() / ".orangutan" / "config.toml";

        const auto result = fixture.registry().execute({
            .id = "cfg_write",
            .name = "write",
            .input = {{"path", "~/.orangutan/config.toml"}, {"content", "[agent]\nmodel = \"gpt\"\n"}},
        });

        expect(not(result.is_error));

        std::ifstream ifs(config_path);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "[agent]\nmodel = \"gpt\"\n");
    };
};

boost::ut::suite builtin_tools_workspace_config_access_test_edit_allows_orangutan_config_outside_workspace_suite = [] {
    "EditAllowsOrangutanConfigOutsideWorkspace"_test = [] {
        BuiltinToolsWorkspaceConfigAccessTest fixture;
        const auto config_path = fixture.home() / ".orangutan" / "config.toml";
        std::ofstream(config_path) << "[agent]\nmodel = \"claude\"\n";

        const std::string patch = "*** ~/.orangutan/config.toml\n"
                                  "<<<<<<< SEARCH\n"
                                  "model = \"claude\"\n"
                                  "=======\n"
                                  "model = \"gpt\"\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({
            .id = "cfg_edit",
            .name = "edit",
            .input = {{"patch", patch}},
        });

        expect(not(result.is_error));

        std::ifstream ifs(config_path);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "[agent]\nmodel = \"gpt\"\n");
    };
};

boost::ut::suite builtin_tools_workspace_config_access_test_home_files_outside_orangutan_config_remain_blocked_suite = [] {
    "HomeFilesOutsideOrangutanConfigRemainBlocked"_test = [] {
        BuiltinToolsWorkspaceConfigAccessTest fixture;
        const auto other_home_file = fixture.home() / "notes.txt";
        std::ofstream(other_home_file) << "private\n";

        const auto result = fixture.registry().execute({
            .id = "cfg_block",
            .name = "read",
            .input = {{"path", "~/notes.txt"}},
        });

        expect(result.is_error);
        expect(result.content.find("workspace sandbox") != std::string::npos);
    };
};

// ── edit tool (patch-based) ─────────────────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_single_file_one_hunk_suite = [] {
    "ApplyPatchSingleFileOneHunk"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        std::ofstream(fixture.workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

        const std::string patch = "*** target.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "    return 0;\n"
                                  "=======\n"
                                  "    return 42;\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap1", .name = "edit", .input = {{"patch", patch}}});
        expect(not(result.is_error));
        expect(result.content.find("1 hunk") != std::string::npos);

        std::ifstream ifs(fixture.workspace() / "target.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "int main() {\n    return 42;\n}\n");
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_single_file_multi_hunk_suite = [] {
    "ApplyPatchSingleFileMultiHunk"_test = [] {
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

        const auto result = fixture.registry().execute({.id = "ap2", .name = "edit", .input = {{"patch", patch}}});
        expect(not(result.is_error));
        expect(result.content.find("2 hunks") != std::string::npos);

        std::ifstream ifs(fixture.workspace() / "multi.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "XXX\nBBB\nZZZ\n");
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_multi_file_suite = [] {
    "ApplyPatchMultiFile"_test = [] {
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

        const auto result = fixture.registry().execute({.id = "ap3", .name = "edit", .input = {{"patch", patch}}});
        expect(not(result.is_error));
        expect(result.content.find("2 files") != std::string::npos);

        {
            std::ifstream ifs(fixture.workspace() / "a.cpp");
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            expect(content == "ALPHA\n");
        }
        {
            std::ifstream ifs(fixture.workspace() / "b.cpp");
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            expect(content == "BETA\n");
        }
    };
};

// ── Parse error tests ───────────────────────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_empty_patch_returns_error_suite = [] {
    "ApplyPatchEmptyPatchReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto result = fixture.registry().execute({.id = "ap_e1", .name = "edit", .input = {{"patch", ""}}});
        expect(result.is_error);
        expect(result.content.find("empty") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_missing_separator_returns_error_suite = [] {
    "ApplyPatchMissingSeparatorReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const std::string patch = "*** file.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "hello\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_e2", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("separator") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_no_file_header_returns_error_suite = [] {
    "ApplyPatchNoFileHeaderReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const std::string patch = "<<<<<<< SEARCH\n"
                                  "hello\n"
                                  "=======\n"
                                  "world\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_e3", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("file header") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_unclosed_hunk_returns_error_suite = [] {
    "ApplyPatchUnclosedHunkReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const std::string patch = "*** file.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "hello\n"
                                  "=======\n"
                                  "world\n";

        const auto result = fixture.registry().execute({.id = "ap_e4", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("unclosed") != std::string::npos);
    };
};

// ── Validation error tests ──────────────────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_search_not_found_returns_error_suite = [] {
    "ApplyPatchSearchNotFoundReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        std::ofstream(fixture.workspace() / "v1.cpp") << "existing content\n";

        const std::string patch = "*** v1.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "nonexistent\n"
                                  "=======\n"
                                  "replacement\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_v1", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("not found") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_search_matches_multiple_returns_error_suite = [] {
    "ApplyPatchSearchMatchesMultipleReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        std::ofstream(fixture.workspace() / "v2.cpp") << "dup\ndup\n";

        const std::string patch = "*** v2.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "dup\n"
                                  "=======\n"
                                  "unique\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_v2", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("multiple") != std::string::npos);
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_file_not_found_returns_error_suite = [] {
    "ApplyPatchFileNotFoundReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const std::string patch = "*** does_not_exist.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "something\n"
                                  "=======\n"
                                  "other\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_v3", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("not found") != std::string::npos);
    };
};

// ── Atomicity test ──────────────────────────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_atomic_rollback_on_failure_suite = [] {
    "ApplyPatchAtomicRollbackOnFailure"_test = [] {
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

        const auto result = fixture.registry().execute({.id = "ap_atom", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);

        // Verify good.cpp was NOT modified (atomic: neither file touched)
        std::ifstream ifs(fixture.workspace() / "good.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "good content\n");
    };
};

// ── New file creation tests ─────────────────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_creates_new_file_suite = [] {
    "ApplyPatchCreatesNewFile"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const std::string patch = "*** newdir/newfile.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "=======\n"
                                  "#include <iostream>\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_new", .name = "edit", .input = {{"patch", patch}}});
        expect(not(result.is_error));
        expect(std::filesystem::exists(fixture.workspace() / "newdir" / "newfile.cpp"));

        std::ifstream ifs(fixture.workspace() / "newdir" / "newfile.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "#include <iostream>");
    };
};

boost::ut::suite builtin_tools_workspace_test_apply_patch_new_file_already_exists_returns_error_suite = [] {
    "ApplyPatchNewFileAlreadyExistsReturnsError"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        std::ofstream(fixture.workspace() / "exists.cpp") << "already here\n";

        const std::string patch = "*** exists.cpp\n"
                                  "<<<<<<< SEARCH\n"
                                  "=======\n"
                                  "new content\n"
                                  ">>>>>>> REPLACE\n";

        const auto result = fixture.registry().execute({.id = "ap_dup", .name = "edit", .input = {{"patch", patch}}});
        expect(result.is_error);
        expect(result.content.find("already exists") != std::string::npos);
    };
};

// ── Integration: registered in registry ─────────

boost::ut::suite builtin_tools_workspace_test_apply_patch_registered_in_registry_suite = [] {
    "ApplyPatchRegisteredInRegistry"_test = [] {
        BuiltinToolsWorkspaceTest fixture;
        const auto defs = fixture.registry().definitions();
        bool found = false;
        for (const auto &def : defs) {
            if (def.name == "edit") {
                found = true;
                expect(not(def.description.empty()));
                break;
            }
        }
        expect(found);
    };
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

boost::ut::suite hashline_tools_test_read_output_has_hash_tags_suite = [] {
    "ReadOutputHasHashTags"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "test.txt") << "hello world\nfoo bar\n";

        const auto result = fixture.registry().execute({.id = "r1", .name = "read", .input = {{"path", "test.txt"}}});
        expect(not(result.is_error));
        expect(result.content.find("1#") != std::string::npos);
        expect(result.content.find(":hello world") != std::string::npos);
        expect(result.content.find("2#") != std::string::npos);
        expect(result.content.find(":foo bar") != std::string::npos);
    };
};

boost::ut::suite hashline_tools_test_read_with_offset_uses_original_line_numbers_suite = [] {
    "ReadWithOffsetUsesOriginalLineNumbers"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "offset.txt") << "line1\nline2\nline3\nline4\n";

        const auto result = fixture.registry().execute({.id = "r2", .name = "read", .input = {{"path", "offset.txt"}, {"offset", 3}, {"limit", 1}}});
        expect(not(result.is_error));
        expect(result.content.find("3#") != std::string::npos);
        expect(result.content.find(":line3") != std::string::npos);
    };
};

boost::ut::suite hashline_tools_test_read_multi_path_has_hash_tags_suite = [] {
    "ReadMultiPathHasHashTags"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "a.txt") << "aaa\n";
        std::ofstream(fixture.workspace() / "b.txt") << "bbb\n";

        const auto result = fixture.registry().execute({.id = "r3", .name = "read", .input = {{"paths", json::array({"a.txt", "b.txt"})}}});
        expect(not(result.is_error));
        expect(result.content.find("=== ") != std::string::npos);
        expect(result.content.find("1#") != std::string::npos);
    };
};

// ── Hashline edit tests ──────────────────────────────

boost::ut::suite hashline_tools_test_edit_replace_single_line_suite = [] {
    "EditReplaceSingleLine"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

        auto hash = orangutan::compute_line_hash("    return 0;", 2);
        std::string anchor = "2#" + hash;

        json edits = json::array({{{"op", "replace"}, {"anchor", anchor}, {"content", json::array({"    return 42;"})}}});

        const auto result = fixture.registry().execute({.id = "e1", .name = "edit", .input = {{"path", "target.cpp"}, {"edits", edits}}});
        expect(not(result.is_error));
        expect(result.content.find("Applied") != std::string::npos);

        std::ifstream ifs(fixture.workspace() / "target.cpp");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "int main() {\n    return 42;\n}\n");
    };
};

boost::ut::suite hashline_tools_test_edit_preserves_missing_final_newline_suite = [] {
    "EditPreservesMissingFinalNewline"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "noeol.txt") << "aaa\nbbb";

        auto hash = orangutan::compute_line_hash("bbb", 2);
        json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", json::array({"ccc"})}}});

        const auto result = fixture.registry().execute({.id = "e1b", .name = "edit", .input = {{"path", "noeol.txt"}, {"edits", edits}}});
        expect(not(result.is_error));

        std::ifstream ifs(fixture.workspace() / "noeol.txt");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "aaa\nccc");
    };
};

boost::ut::suite hashline_tools_test_edit_delete_line_suite = [] {
    "EditDeleteLine"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "del.txt") << "aaa\nbbb\nccc\n";

        auto hash = orangutan::compute_line_hash("bbb", 2);
        json edits = json::array({{{"op", "delete"}, {"anchor", "2#" + hash}}});

        const auto result = fixture.registry().execute({.id = "e2", .name = "edit", .input = {{"path", "del.txt"}, {"edits", edits}}});
        expect(not(result.is_error));

        std::ifstream ifs(fixture.workspace() / "del.txt");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "aaa\nccc\n");
    };
};

boost::ut::suite hashline_tools_test_edit_insert_after_eof_suite = [] {
    "EditInsertAfterEOF"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "ins.txt") << "aaa\nbbb\n";

        json edits = json::array({{{"op", "insert_after"}, {"content", json::array({"ccc"})}}});

        const auto result = fixture.registry().execute({.id = "e3", .name = "edit", .input = {{"path", "ins.txt"}, {"edits", edits}}});
        expect(not(result.is_error));

        std::ifstream ifs(fixture.workspace() / "ins.txt");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "aaa\nbbb\nccc\n");
    };
};

boost::ut::suite hashline_tools_test_edit_hash_mismatch_returns_error_with_context_suite = [] {
    "EditHashMismatchReturnsErrorWithContext"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "stale.txt") << "aaa\nbbb\nccc\n";

        auto actual_hash = orangutan::compute_line_hash("bbb", 2);
        const std::string wrong_hash = actual_hash == "ZZ" ? "ZY" : "ZZ";
        json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + wrong_hash}, {"content", json::array({"XXX"})}}});

        const auto result = fixture.registry().execute({.id = "e4", .name = "edit", .input = {{"path", "stale.txt"}, {"edits", edits}}});
        expect(result.is_error);
        expect(result.content.find("mismatch") != std::string::npos);
        expect(result.content.find(actual_hash) != std::string::npos);
    };
};

boost::ut::suite hashline_tools_test_edit_content_as_string_is_split_on_newlines_suite = [] {
    "EditContentAsStringIsSplitOnNewlines"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "str.txt") << "aaa\nbbb\n";

        auto hash = orangutan::compute_line_hash("bbb", 2);
        json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", "line1\nline2"}}});

        const auto result = fixture.registry().execute({.id = "e5", .name = "edit", .input = {{"path", "str.txt"}, {"edits", edits}}});
        expect(not(result.is_error));

        std::ifstream ifs(fixture.workspace() / "str.txt");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(content == "aaa\nline1\nline2\n");
    };
};

boost::ut::suite hashline_tools_test_edit_missing_anchor_for_replace_returns_error_suite = [] {
    "EditMissingAnchorForReplaceReturnsError"_test = [] {
        HashlineToolsTest fixture;
        std::ofstream(fixture.workspace() / "missing.txt") << "aaa\n";

        json edits = json::array({{{"op", "replace"}, {"content", json::array({"XXX"})}}});

        const auto result = fixture.registry().execute({.id = "e6", .name = "edit", .input = {{"path", "missing.txt"}, {"edits", edits}}});
        expect(result.is_error);
        expect(result.content.find("requires") != std::string::npos);
    };
};

// ── Mode switching: schema differs between modes ──

boost::ut::suite hashline_tools_test_edit_tool_description_mentions_hash_anchors_suite = [] {
    "EditToolDescriptionMentionsHashAnchors"_test = [] {
        HashlineToolsTest fixture;
        const auto defs = fixture.registry().definitions();
        for (const auto &def : defs) {
            if (def.name == "edit") {
                expect(def.description.find("hash") != std::string::npos);
                expect(def.input_schema.contains("properties"));
                expect(def.input_schema["properties"].contains("edits"));
                return;
            }
        }
        expect(false >> fatal) << "edit tool not found in registry";
    };
};

boost::ut::suite mode_switch_search_replace_edit_tool_description_mentions_patch_suite = [] {
    "SearchReplaceEditToolDescriptionMentionsPatch"_test = [] {
        ToolRegistry sr_registry;
        auto sr_workspace = orangutan::testing::test_tmp_root() / "orangutan_sr_mode_test";
        std::filesystem::create_directories(sr_workspace);
        register_builtin_tools(sr_registry, nullptr, sr_workspace.string(), nullptr, nullptr, "search_replace");

        const auto defs = sr_registry.definitions();
        for (const auto &def : defs) {
            if (def.name == "edit") {
                expect(def.description.find("patch") != std::string::npos);
                expect(def.input_schema.contains("properties"));
                expect(def.input_schema["properties"].contains("patch"));
                std::filesystem::remove_all(sr_workspace);
                return;
            }
        }
        std::filesystem::remove_all(sr_workspace);
        expect(false >> fatal) << "edit tool not found in registry";
    };
};
