#include "features/hooks/hook-manager.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <magic_enum/magic_enum.hpp>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("discovers_hooks_from_directory") {
    const auto fixtures = fixtures_dir();
    ensure_fixture_scripts_are_executable(fixtures);

    HookManager manager;
    manager.load_from_directories({fixtures.string()});

    CHECK(manager.hook_count(HookEvent::before_tool_call) == 4ul);
    CHECK(manager.hook_count(HookEvent::after_tool_call) == 2ul);
    CHECK(manager.hook_count(HookEvent::session_start) == 1ul);
    CHECK(manager.hook_count(HookEvent::session_end) == 1ul);
    CHECK(manager.total_hooks() >= 8ul);
};

TEST_CASE("empty_directory_has_no_hooks") {
    HookManager manager;
    TempDirGuard temp_dir{"empty-hooks"};
    std::filesystem::create_directories(temp_dir.root);

    manager.load_from_directories({temp_dir.string()});

    CHECK(manager.total_hooks() == 0ul);
};

TEST_CASE("nonexistent_directory_has_no_hooks") {
    HookManager manager;

    manager.load_from_directories({orangutan::testing::unique_test_path("nonexistent-hooks").string()});

    CHECK(manager.total_hooks() == 0ul);
};

TEST_CASE("skips_non_executable_files") {
    TempDirGuard temp_dir{"nonexec-hooks"};
    write_script(temp_dir.root / "before_tool_call" / "not-exec.sh", "#!/bin/sh\nexit 0\n", false);

    HookManager manager;
    manager.load_from_directories({temp_dir.string()});

    CHECK(manager.hook_count(HookEvent::before_tool_call) == 0ul);
};

TEST_CASE("ignores_unknown_event_directories") {
    TempDirGuard temp_dir{"unknown-hooks"};
    std::filesystem::create_directories(temp_dir.root / "unknown_event");

    HookManager manager;
    manager.load_from_directories({temp_dir.string()});

    CHECK(manager.total_hooks() == 0ul);
};

TEST_CASE("before_tool_call_allows_when_all_hooks_pass") {
    TempDirGuard temp_dir{"allow-hooks"};
    write_script(temp_dir.root / "before_tool_call" / "01-allow.sh", "#!/bin/sh\nexit 0\n");

    HookManager manager;
    manager.load_from_directories({temp_dir.string()});

    const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

    CHECK(result.allowed);
};

TEST_CASE("before_tool_call_blocks_on_non_zero_exit") {
    TempDirGuard temp_dir{"block-hooks"};
    write_script(temp_dir.root / "before_tool_call" / "01-block.sh", "#!/bin/sh\necho 'forbidden' >&2\nexit 1\n");

    HookManager manager;
    manager.load_from_directories({temp_dir.string()});

    const auto ctx = build_before_tool_call_context("shell", {{"command", "rm -rf /"}});
    const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

    CHECK_FALSE(result.allowed);
    CHECK(result.blocked_by == "01-block.sh");
    CHECK(result.block_reason.contains("forbidden"));
};

TEST_CASE("after_tool_call_is_non_blocking_on_failure") {
    const auto fixtures = fixtures_dir();
    ensure_fixture_scripts_are_executable(fixtures);

    HookManager manager;
    manager.load_from_directories({fixtures.string()});

    const auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "output", false);
    const auto result = manager.dispatch(HookEvent::after_tool_call, ctx);

    CHECK(result.allowed);
};

TEST_CASE("missing_event_hooks_return_allowed") {
    HookManager manager;

    const auto ctx = build_message_context(HookEvent::message_received, "user", "hello");
    const auto result = manager.dispatch(HookEvent::message_received, ctx);

    CHECK(result.allowed);
};

TEST_CASE("workspace_hook_shadows_global_by_filename") {
    TempDirGuard global_dir{"shadow-global"};
    write_script(global_dir.root / "before_tool_call" / "01-hook.sh", "#!/bin/sh\nexit 1\n");

    TempDirGuard workspace_dir{"shadow-workspace"};
    write_script(workspace_dir.root / "before_tool_call" / "01-hook.sh", "#!/bin/sh\nexit 0\n");

    HookManager manager;
    manager.load_from_directories({global_dir.string(), workspace_dir.string()});

    CHECK(manager.hook_count(HookEvent::before_tool_call) == 1ul);

    const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

    CHECK(result.allowed);
};

TEST_CASE("hook_path_with_spaces_executes_directly") {
    TempDirGuard temp_dir{"space-hook"};
    write_script(temp_dir.root / "before_tool_call" / "01 hook.sh", "#!/bin/sh\necho 'spaced hook' >&2\nexit 1\n");

    HookManager manager;
    manager.load_from_directories({temp_dir.string()});

    const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    const auto result = manager.dispatch(HookEvent::before_tool_call, ctx);

    CHECK_FALSE(result.allowed);
    CHECK(result.blocked_by == "01 hook.sh");
    CHECK(result.block_reason.contains("spaced hook"));
};

TEST_CASE("shadowing_preserves_directory_order_for_distinct_hooks") {
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

    CHECK(manager.hook_count(HookEvent::after_tool_call) == 3ul);

    const auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "ok", false);
    const auto result = manager.dispatch(HookEvent::after_tool_call, ctx);

    CHECK(result.allowed);
    CHECK(read_file(log_path) == "global\nworkspace\nnew-shared\n");

    std::filesystem::remove(log_path);
};

TEST_CASE("build_before_tool_call_context_has_correct_fields") {
    const auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});

    CHECK(ctx["event"] == "before_tool_call");
    CHECK(ctx.contains("timestamp"));
    CHECK(ctx["tool_name"] == "shell");
    CHECK(ctx["tool_input"]["command"] == "ls");
    CHECK_FALSE(ctx.contains("tool_result"));
};

TEST_CASE("build_after_tool_call_context_has_result_fields") {
    const auto ctx = build_after_tool_call_context("read", {{"path", "f.txt"}}, "file contents", false);

    CHECK(ctx["event"] == "after_tool_call");
    CHECK(ctx["tool_result"] == "file contents");
    CHECK(ctx["is_error"] == false);
};

TEST_CASE("build_message_context") {
    const auto ctx = build_message_context(HookEvent::message_received, "user", "hello world");

    CHECK(ctx["event"] == "message_received");
    CHECK(ctx["role"] == "user");
    CHECK(ctx["content"] == "hello world");
    CHECK(ctx.contains("timestamp"));
};

TEST_CASE("build_session_context_start") {
    const auto ctx = build_session_context(HookEvent::session_start, "sess-123");

    CHECK(ctx["event"] == "session_start");
    CHECK(ctx["session_id"] == "sess-123");
    CHECK_FALSE(ctx.contains("message_count"));
};

TEST_CASE("build_session_context_end") {
    const auto ctx = build_session_context(HookEvent::session_end, "sess-123", 42);

    CHECK(ctx["event"] == "session_end");
    CHECK(ctx["session_id"] == "sess-123");
    CHECK(ctx["message_count"] == 42);
};

TEST_CASE("event_names_match_enum_tokens_in_public_contexts") {
    CHECK(build_before_tool_call_context("shell", {{"command", "ls"}})["event"] == std::string{magic_enum::enum_name(HookEvent::before_tool_call)});
    CHECK(build_message_context(HookEvent::message_received, "user", "hello world")["event"] == std::string{magic_enum::enum_name(HookEvent::message_received)});
    CHECK(build_session_context(HookEvent::session_end, "sess-123", 42)["event"] == std::string{magic_enum::enum_name(HookEvent::session_end)});
};

} // namespace
