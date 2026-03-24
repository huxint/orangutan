#include "features/hooks/hook-manager.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "support/ut.hpp"

using namespace orangutan;

namespace {

std::filesystem::path fixtures_dir() {
    return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/hooks";
}

void ensure_fixture_scripts_are_executable(const std::filesystem::path &root) {
    if (!std::filesystem::exists(root)) {
        return;
    }

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }
}

struct TempDirGuard {
    explicit TempDirGuard(std::string_view name)
    : root(orangutan::testing::unique_test_root(name)) {}

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    [[nodiscard]]
    std::string string() const {
        return root.string();
    }

    std::filesystem::path root;
};

void write_script(const std::filesystem::path &path, const std::string &content, bool executable = true) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(path);
    out << content;
    out.close();

    if (executable) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

boost::ut::suite hook_manager_suite = [] {
    using namespace boost::ut;

    "discovers_hooks_from_directory"_test = [] {
        const auto fixtures = fixtures_dir();
        ensure_fixture_scripts_are_executable(fixtures);

        HookManager manager;
        manager.load_from_directories({fixtures.string()});

        expect(manager.hook_count(HookEvent::before_tool_call) == 4_ul);
        expect(manager.hook_count(HookEvent::after_tool_call) == 2_ul);
        expect(manager.hook_count(HookEvent::session_start) == 1_ul);
        expect(manager.hook_count(HookEvent::session_end) == 1_ul);
        expect(manager.total_hooks() >= 8_ul);
    };

    "empty_directory_has_no_hooks"_test = [] {
        HookManager manager;
        TempDirGuard temp_dir{"empty-hooks"};
        std::filesystem::create_directories(temp_dir.root);

        manager.load_from_directories({temp_dir.string()});

        expect(manager.total_hooks() == 0_ul);
    };

    "nonexistent_directory_has_no_hooks"_test = [] {
        HookManager manager;

        manager.load_from_directories({orangutan::testing::unique_test_path("nonexistent-hooks").string()});

        expect(manager.total_hooks() == 0_ul);
    };

    "skips_non_executable_files"_test = [] {
        TempDirGuard temp_dir{"nonexec-hooks"};
        write_script(temp_dir.root / "before_tool_call" / "not-exec.sh", "#!/bin/sh\nexit 0\n", false);

        HookManager manager;
        manager.load_from_directories({temp_dir.string()});

        expect(manager.hook_count(HookEvent::before_tool_call) == 0_ul);
    };

    "ignores_unknown_event_directories"_test = [] {
        TempDirGuard temp_dir{"unknown-hooks"};
        std::filesystem::create_directories(temp_dir.root / "unknown_event");

        HookManager manager;
        manager.load_from_directories({temp_dir.string()});

        expect(manager.total_hooks() == 0_ul);
    };

    "before_tool_call_allows_when_all_hooks_pass"_test = [] {
        TempDirGuard temp_dir{"allow-hooks"};
        write_script(temp_dir.root / "before_tool_call" / "01-allow.sh", "#!/bin/sh\nexit 0\n");

        HookManager manager;
        manager.load_from_directories({temp_dir.string()});

        const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
        const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

        expect(result.allowed);
    };

    "before_tool_call_blocks_on_non_zero_exit"_test = [] {
        TempDirGuard temp_dir{"block-hooks"};
        write_script(temp_dir.root / "before_tool_call" / "01-block.sh", "#!/bin/sh\necho 'forbidden' >&2\nexit 1\n");

        HookManager manager;
        manager.load_from_directories({temp_dir.string()});

        const auto ctx = build_before_tool_call_context("shell", {{"command", "rm -rf /"}});
        const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

        expect(not result.allowed);
        expect(result.blocked_by == "01-block.sh");
        expect(result.block_reason.find("forbidden") != std::string::npos);
    };

    "after_tool_call_is_non_blocking_on_failure"_test = [] {
        const auto fixtures = fixtures_dir();
        ensure_fixture_scripts_are_executable(fixtures);

        HookManager manager;
        manager.load_from_directories({fixtures.string()});

        const auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "output", false);
        const auto result = manager.dispatch(HookEvent::after_tool_call, ctx);

        expect(result.allowed);
    };

    "missing_event_hooks_return_allowed"_test = [] {
        HookManager manager;

        const auto ctx = build_message_context(HookEvent::message_received, "user", "hello");
        const auto result = manager.dispatch(HookEvent::message_received, ctx);

        expect(result.allowed);
    };

    "workspace_hook_shadows_global_by_filename"_test = [] {
        TempDirGuard global_dir{"shadow-global"};
        write_script(global_dir.root / "before_tool_call" / "01-hook.sh", "#!/bin/sh\nexit 1\n");

        TempDirGuard workspace_dir{"shadow-workspace"};
        write_script(workspace_dir.root / "before_tool_call" / "01-hook.sh", "#!/bin/sh\nexit 0\n");

        HookManager manager;
        manager.load_from_directories({global_dir.string(), workspace_dir.string()});

        expect(manager.hook_count(HookEvent::before_tool_call) == 1_ul);

        const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
        const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

        expect(result.allowed);
    };

    "hook_path_with_spaces_executes_directly"_test = [] {
        TempDirGuard temp_dir{"space-hook"};
        write_script(temp_dir.root / "before_tool_call" / "01 hook.sh", "#!/bin/sh\necho 'spaced hook' >&2\nexit 1\n");

        HookManager manager;
        manager.load_from_directories({temp_dir.string()});

        const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
        const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

        expect(not result.allowed);
        expect(result.blocked_by == "01 hook.sh");
        expect(result.block_reason.find("spaced hook") != std::string::npos);
    };

    "shadowing_preserves_directory_order_for_distinct_hooks"_test = [] {
        TempDirGuard global_dir{"order-global"};
        TempDirGuard workspace_dir{"order-workspace"};
        const auto log_path = orangutan::testing::unique_test_path("hook-order-log", "order.log");
        std::filesystem::remove(log_path);

        write_script(global_dir.root / "after_tool_call" / "02-global.sh", "#!/bin/sh\nprintf 'global\\n' >> " + log_path.string() + "\nexit 0\n");
        write_script(global_dir.root / "after_tool_call" / "03-shared.sh", "#!/bin/sh\nprintf 'old-shared\\n' >> " + log_path.string() + "\nexit 0\n");
        write_script(workspace_dir.root / "after_tool_call" / "01-workspace.sh", "#!/bin/sh\nprintf 'workspace\\n' >> " + log_path.string() + "\nexit 0\n");
        write_script(workspace_dir.root / "after_tool_call" / "03-shared.sh", "#!/bin/sh\nprintf 'new-shared\\n' >> " + log_path.string() + "\nexit 0\n");

        HookManager manager;
        manager.load_from_directories({global_dir.string(), workspace_dir.string()});

        expect(manager.hook_count(HookEvent::after_tool_call) == 3_ul);

        const auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "ok", false);
        const auto result = manager.dispatch(HookEvent::after_tool_call, ctx);

        expect(result.allowed);
        expect(read_file(log_path) == "global\nworkspace\nnew-shared\n");

        std::filesystem::remove(log_path);
    };

    "build_before_tool_call_context_has_correct_fields"_test = [] {
        const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});

        expect(ctx["event"] == "before_tool_call");
        expect(ctx.contains("timestamp"));
        expect(ctx["tool_name"] == "shell");
        expect(ctx["tool_input"]["command"] == "ls");
        expect(not ctx.contains("tool_result"));
    };

    "build_after_tool_call_context_has_result_fields"_test = [] {
        const auto ctx = build_after_tool_call_context("read", {{"path", "f.txt"}}, "file contents", false);

        expect(ctx["event"] == "after_tool_call");
        expect(ctx["tool_result"] == "file contents");
        expect(ctx["is_error"] == false);
    };

    "build_message_context"_test = [] {
        const auto ctx = build_message_context(HookEvent::message_received, "user", "hello world");

        expect(ctx["event"] == "message_received");
        expect(ctx["role"] == "user");
        expect(ctx["content"] == "hello world");
        expect(ctx.contains("timestamp"));
    };

    "build_session_context_start"_test = [] {
        const auto ctx = build_session_context(HookEvent::session_start, "sess-123");

        expect(ctx["event"] == "session_start");
        expect(ctx["session_id"] == "sess-123");
        expect(not ctx.contains("message_count"));
    };

    "build_session_context_end"_test = [] {
        const auto ctx = build_session_context(HookEvent::session_end, "sess-123", 42);

        expect(ctx["event"] == "session_end");
        expect(ctx["session_id"] == "sess-123");
        expect(ctx["message_count"] == 42);
    };

    "event_to_string"_test = [] {
        expect(hook_event_to_string(HookEvent::before_tool_call) == "before_tool_call");
        expect(hook_event_to_string(HookEvent::session_end) == "session_end");
    };
};

} // namespace
