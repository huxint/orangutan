#include "features/hooks/hook-manager.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

using namespace orangutan;

std::filesystem::path fixtures_dir() {
    return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/hooks";
}

namespace {

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

} // namespace

// ── Hook discovery ──────────────────────────────

TEST(HookManagerTest, DiscoversHooksFromDirectory) {
    const auto fixtures = fixtures_dir();
    ensure_fixture_scripts_are_executable(fixtures);

    HookManager manager;
    manager.load_from_directories({fixtures.string()});

    EXPECT_EQ(manager.hook_count(HookEvent::before_tool_call), 4);
    EXPECT_EQ(manager.hook_count(HookEvent::after_tool_call), 2);
    EXPECT_EQ(manager.hook_count(HookEvent::session_start), 1);
    EXPECT_EQ(manager.hook_count(HookEvent::session_end), 1);
    EXPECT_GE(manager.total_hooks(), 8);
}

TEST(HookManagerTest, EmptyDirectoryNoHooks) {
    HookManager manager;
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_empty_hooks";
    std::filesystem::create_directories(tmp);
    manager.load_from_directories({tmp.string()});
    EXPECT_EQ(manager.total_hooks(), 0);
    std::filesystem::remove_all(tmp);
}

TEST(HookManagerTest, NonexistentDirectoryNoHooks) {
    HookManager manager;
    manager.load_from_directories({"/tmp/orangutan_nonexistent_hooks_xyz"});
    EXPECT_EQ(manager.total_hooks(), 0);
}

TEST(HookManagerTest, SkipsNonExecutableFiles) {
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_nonexec_hooks";
    std::filesystem::create_directories(tmp / "before_tool_call");
    {
        std::ofstream(tmp / "before_tool_call" / "not-exec.sh") << "#!/bin/sh\nexit 0\n";
        // Deliberately NOT making it executable
    }

    HookManager manager;
    manager.load_from_directories({tmp.string()});
    EXPECT_EQ(manager.hook_count(HookEvent::before_tool_call), 0);

    std::filesystem::remove_all(tmp);
}

TEST(HookManagerTest, IgnoresUnknownEventDirectories) {
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_unknown_hooks";
    std::filesystem::create_directories(tmp / "unknown_event");

    HookManager manager;
    manager.load_from_directories({tmp.string()});
    EXPECT_EQ(manager.total_hooks(), 0);

    std::filesystem::remove_all(tmp);
}

// ── Hook dispatch ───────────────────────────────

TEST(HookManagerTest, BeforeToolCallAllowsWhenAllHooksPass) {
    // Create a temporary directory with only an allow hook
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_allow_hooks";
    std::filesystem::create_directories(tmp / "before_tool_call");
    {
        std::ofstream(tmp / "before_tool_call" / "01-allow.sh") << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(tmp / "before_tool_call" / "01-allow.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    HookManager manager;
    manager.load_from_directories({tmp.string()});

    auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    auto result = manager.dispatch(HookEvent::before_tool_call, ctx);
    EXPECT_TRUE(result.allowed);

    std::filesystem::remove_all(tmp);
}

TEST(HookManagerTest, BeforeToolCallBlocksOnNonZeroExit) {
    // Create a temporary directory with only a blocking hook
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_block_hooks";
    std::filesystem::create_directories(tmp / "before_tool_call");
    {
        std::ofstream(tmp / "before_tool_call" / "01-block.sh") << "#!/bin/sh\necho 'forbidden' >&2\nexit 1\n";
    }
    std::filesystem::permissions(tmp / "before_tool_call" / "01-block.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    HookManager manager;
    manager.load_from_directories({tmp.string()});

    auto ctx = build_before_tool_call_context("shell", {{"command", "rm -rf /"}});
    auto result = manager.dispatch(HookEvent::before_tool_call, ctx);
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.blocked_by, "01-block.sh");
    EXPECT_NE(result.block_reason.find("forbidden"), std::string::npos);

    std::filesystem::remove_all(tmp);
}

TEST(HookManagerTest, AfterToolCallNonBlockingOnFailure) {
    const auto fixtures = fixtures_dir();
    ensure_fixture_scripts_are_executable(fixtures);

    HookManager manager;
    manager.load_from_directories({fixtures.string()});

    auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "output", false);
    // after_tool_call hooks are non-blocking even on non-zero exit
    auto result = manager.dispatch(HookEvent::after_tool_call, ctx);
    EXPECT_TRUE(result.allowed);
}

TEST(HookManagerTest, NoHooksForEventReturnsAllowed) {
    HookManager manager;
    // No hooks loaded at all
    auto ctx = build_message_context(HookEvent::message_received, "user", "hello");
    auto result = manager.dispatch(HookEvent::message_received, ctx);
    EXPECT_TRUE(result.allowed);
}

// ── Hook shadowing ──────────────────────────────

TEST(HookManagerTest, WorkspaceHookShadowsGlobalByFilename) {
    // Global dir: 01-hook.sh exits 1 (blocks)
    auto global_dir = std::filesystem::temp_directory_path() / "orangutan_shadow_global";
    std::filesystem::create_directories(global_dir / "before_tool_call");
    {
        std::ofstream(global_dir / "before_tool_call" / "01-hook.sh") << "#!/bin/sh\nexit 1\n";
    }
    std::filesystem::permissions(global_dir / "before_tool_call" / "01-hook.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    // Workspace dir: 01-hook.sh exits 0 (allows) — should override global
    auto workspace_dir = std::filesystem::temp_directory_path() / "orangutan_shadow_workspace";
    std::filesystem::create_directories(workspace_dir / "before_tool_call");
    {
        std::ofstream(workspace_dir / "before_tool_call" / "01-hook.sh") << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(workspace_dir / "before_tool_call" / "01-hook.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    HookManager manager;
    // Global first, then workspace — workspace should shadow
    manager.load_from_directories({global_dir.string(), workspace_dir.string()});

    EXPECT_EQ(manager.hook_count(HookEvent::before_tool_call), 1); // not 2

    auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    auto result = manager.dispatch(HookEvent::before_tool_call, ctx);
    EXPECT_TRUE(result.allowed); // workspace override allows

    std::filesystem::remove_all(global_dir);
    std::filesystem::remove_all(workspace_dir);
}

TEST(HookManagerTest, HookPathWithSpacesExecutesDirectly) {
    auto tmp = std::filesystem::temp_directory_path() / "orangutan_space_hook";
    std::filesystem::create_directories(tmp / "before_tool_call");
    {
        std::ofstream(tmp / "before_tool_call" / "01 hook.sh") << "#!/bin/sh\necho 'spaced hook' >&2\nexit 1\n";
    }
    std::filesystem::permissions(tmp / "before_tool_call" / "01 hook.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    HookManager manager;
    manager.load_from_directories({tmp.string()});

    auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    auto result = manager.dispatch(HookEvent::before_tool_call, ctx);
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.blocked_by, "01 hook.sh");
    EXPECT_NE(result.block_reason.find("spaced hook"), std::string::npos);

    std::filesystem::remove_all(tmp);
}

TEST(HookManagerTest, ShadowingPreservesDirectoryOrderForDistinctHooks) {
    auto global_dir = std::filesystem::temp_directory_path() / "orangutan_order_global";
    auto workspace_dir = std::filesystem::temp_directory_path() / "orangutan_order_workspace";
    auto log_path = std::filesystem::temp_directory_path() / "orangutan_hook_order.log";
    std::filesystem::remove(log_path);

    std::filesystem::create_directories(global_dir / "after_tool_call");
    {
        std::ofstream(global_dir / "after_tool_call" / "02-global.sh") << "#!/bin/sh\nprintf 'global\\n' >> " << log_path.string() << "\nexit 0\n";
        std::ofstream(global_dir / "after_tool_call" / "03-shared.sh") << "#!/bin/sh\nprintf 'old-shared\\n' >> " << log_path.string() << "\nexit 0\n";
    }
    std::filesystem::permissions(global_dir / "after_tool_call" / "02-global.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::filesystem::permissions(global_dir / "after_tool_call" / "03-shared.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    std::filesystem::create_directories(workspace_dir / "after_tool_call");
    {
        std::ofstream(workspace_dir / "after_tool_call" / "01-workspace.sh") << "#!/bin/sh\nprintf 'workspace\\n' >> " << log_path.string() << "\nexit 0\n";
        std::ofstream(workspace_dir / "after_tool_call" / "03-shared.sh") << "#!/bin/sh\nprintf 'new-shared\\n' >> " << log_path.string() << "\nexit 0\n";
    }
    std::filesystem::permissions(workspace_dir / "after_tool_call" / "01-workspace.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::filesystem::permissions(workspace_dir / "after_tool_call" / "03-shared.sh",
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    HookManager manager;
    manager.load_from_directories({global_dir.string(), workspace_dir.string()});

    EXPECT_EQ(manager.hook_count(HookEvent::after_tool_call), 3);

    auto ctx = build_after_tool_call_context("shell", {{"command", "ls"}}, "ok", false);
    auto result = manager.dispatch(HookEvent::after_tool_call, ctx);
    EXPECT_TRUE(result.allowed);

    std::ifstream log_file(log_path);
    std::string log((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(log, "global\nworkspace\nnew-shared\n");

    std::filesystem::remove(log_path);
    std::filesystem::remove_all(global_dir);
    std::filesystem::remove_all(workspace_dir);
}

// ── Context builders ────────────────────────────

TEST(HookManagerTest, BuildBeforeToolCallContextHasCorrectFields) {
    auto ctx = build_before_tool_call_context("shell", {{"command", "ls"}});
    EXPECT_EQ(ctx["event"], "before_tool_call");
    EXPECT_TRUE(ctx.contains("timestamp"));
    EXPECT_EQ(ctx["tool_name"], "shell");
    EXPECT_EQ(ctx["tool_input"]["command"], "ls");
    EXPECT_FALSE(ctx.contains("tool_result"));
}

TEST(HookManagerTest, BuildAfterToolCallContextHasResultFields) {
    auto ctx = build_after_tool_call_context("read", {{"path", "f.txt"}}, "file contents", false);
    EXPECT_EQ(ctx["event"], "after_tool_call");
    EXPECT_EQ(ctx["tool_result"], "file contents");
    EXPECT_EQ(ctx["is_error"], false);
}

TEST(HookManagerTest, BuildMessageContext) {
    auto ctx = build_message_context(HookEvent::message_received, "user", "hello world");
    EXPECT_EQ(ctx["event"], "message_received");
    EXPECT_EQ(ctx["role"], "user");
    EXPECT_EQ(ctx["content"], "hello world");
    EXPECT_TRUE(ctx.contains("timestamp"));
}

TEST(HookManagerTest, BuildSessionContextStart) {
    auto ctx = build_session_context(HookEvent::session_start, "sess-123");
    EXPECT_EQ(ctx["event"], "session_start");
    EXPECT_EQ(ctx["session_id"], "sess-123");
    EXPECT_FALSE(ctx.contains("message_count"));
}

TEST(HookManagerTest, BuildSessionContextEnd) {
    auto ctx = build_session_context(HookEvent::session_end, "sess-123", 42);
    EXPECT_EQ(ctx["event"], "session_end");
    EXPECT_EQ(ctx["session_id"], "sess-123");
    EXPECT_EQ(ctx["message_count"], 42);
}

// ── Event string conversion ─────────────────────

TEST(HookManagerTest, EventToString) {
    EXPECT_EQ(hook_event_to_string(HookEvent::before_tool_call), "before_tool_call");
    EXPECT_EQ(hook_event_to_string(HookEvent::session_end), "session_end");
}
