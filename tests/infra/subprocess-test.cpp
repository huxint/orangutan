#include "infra/subprocess/subprocess.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

using namespace orangutan;

// ── Basic execution ─────────────────────────────

TEST(SubprocessTest, EchoCommand) {
    auto result = run_subprocess({.command = "echo hello"});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_output, "hello\n");
    EXPECT_TRUE(result.stderr_output.empty());
    EXPECT_FALSE(result.timed_out);
}

TEST(SubprocessTest, CapturesStdout) {
    auto result = run_subprocess({.command = "printf 'line1\nline2\n'"});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_output, "line1\nline2\n");
}

TEST(SubprocessTest, CapturesStderr) {
    auto result = run_subprocess({.command = "echo err >&2"});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stderr_output, "err\n");
}

TEST(SubprocessTest, StdinDelivery) {
    auto result = run_subprocess({.command = "cat", .stdin_data = "hello from stdin"});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_output, "hello from stdin");
}

TEST(SubprocessTest, NonZeroExitCode) {
    auto result = run_subprocess({.command = "exit 42"});
    EXPECT_EQ(result.exit_code, 42);
    EXPECT_FALSE(result.timed_out);
}

TEST(SubprocessTest, CommandNotFound) {
    auto result = run_subprocess({.command = "nonexistent_command_xyz_12345"});
    EXPECT_EQ(result.exit_code, 127);
}

// ── Timeout enforcement ─────────────────────────

TEST(SubprocessTest, TimeoutKillsQuietChild) {
    auto start = std::chrono::steady_clock::now();
    auto result = run_subprocess({.command = "sleep 999", .timeout = std::chrono::seconds(1)});
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.timed_out);
    EXPECT_EQ(result.exit_code, -1);
    // Should complete in ~1s, not 999s
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5);
}

TEST(SubprocessTest, TimeoutStillAppliesAfterChildClosesOutput) {
    auto start = std::chrono::steady_clock::now();
    auto result = run_subprocess({.command = "exec >/dev/null 2>&1; sleep 999", .timeout = std::chrono::seconds(1)});
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.timed_out);
    EXPECT_EQ(result.exit_code, -1);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5);
}

// ── Working directory ───────────────────────────

TEST(SubprocessTest, WorkingDirectory) {
    auto result = run_subprocess({.command = "pwd", .working_dir = "/tmp"});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_output, "/tmp\n");
}

// ── Pipe deadlock regression ────────────────────

TEST(SubprocessTest, LargeStderrDoesNotDeadlock) {
    // Child writes > 64KB to stderr while also writing to stdout.
    // Without poll()-based multiplexing, this would deadlock.
    auto result = run_subprocess({
        .command = "python3 -c \"import sys; sys.stderr.write('E'*100000); sys.stdout.write('O'*100000)\"",
        .timeout = std::chrono::seconds(10),
    });

    // If we get here, no deadlock occurred
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.stdout_output.size(), 100000);
    EXPECT_EQ(result.stderr_output.size(), 100000);
}

TEST(SubprocessTest, LargeStdinAndStdoutDoNotDeadlock) {
    std::string input(100000, 'I');
    auto result = run_subprocess({
        .command = "python3 -c \"import sys; sys.stdout.write('O'*100000); sys.stdout.flush(); data=sys.stdin.read(); sys.stderr.write(str(len(data)))\"",
        .stdin_data = input,
        .timeout = std::chrono::seconds(10),
    });

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.stdout_output.size(), 100000);
    EXPECT_EQ(result.stderr_output, "100000");
}
