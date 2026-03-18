#include "infra/subprocess/subprocess.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <thread>

using namespace orangutan;

namespace {

std::filesystem::path test_tmp_root() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "tmp" / "tests";
    std::filesystem::create_directories(root);
    return root;
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char *name, const std::string &value)
    : name_(name) {
        if (const auto *current = std::getenv(name); current != nullptr) {
            had_previous_ = true;
            previous_ = current;
        }
        setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (had_previous_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;
    ScopedEnvVar(ScopedEnvVar &&) = delete;
    ScopedEnvVar &operator=(ScopedEnvVar &&) = delete;

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

} // namespace

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
    const auto working_dir = test_tmp_root() / "subprocess-working-dir";
    std::filesystem::remove_all(working_dir);
    std::filesystem::create_directories(working_dir);

    auto result = run_subprocess({.command = "pwd", .working_dir = working_dir.string()});
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_output, working_dir.string() + "\n");

    std::filesystem::remove_all(working_dir);
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

TEST(SubprocessTest, BackgroundManagerCapturesOutputAndExit) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    BackgroundProcessManager manager;
    const auto summary = manager.start(
        {
            .command = "python3 -c \"import time; print('tick', flush=True); time.sleep(0.2); print('tock', flush=True)\"",
            .use_shell = true,
        },
        "python3 background test");

    EXPECT_FALSE(summary.process_id.empty());
    EXPECT_TRUE(summary.running);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    BackgroundProcessSnapshot snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = manager.poll(summary.process_id);
        if (!snapshot.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exit_code, 0);
    EXPECT_NE(snapshot.stdout_output.find("tick"), std::string::npos);
    EXPECT_NE(snapshot.stdout_output.find("tock"), std::string::npos);
}

TEST(SubprocessTest, BackgroundManagerKillStopsRunningProcess) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    BackgroundProcessManager manager;
    const auto summary = manager.start(
        {
            .command = "python3 -c \"import time; time.sleep(10)\"",
            .use_shell = true,
        },
        "python3 kill test");

    const auto snapshot = manager.kill(summary.process_id);
    EXPECT_FALSE(snapshot.running);
    EXPECT_TRUE(snapshot.kill_requested);
    EXPECT_TRUE(snapshot.signal_number.has_value());
}

TEST(SubprocessTest, BackgroundManagerKillAfterCompletionDoesNotDeadlock) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    BackgroundProcessManager manager;
    const auto summary = manager.start(
        {
            .command = "printf 'done\\n'",
            .use_shell = true,
        },
        "completed process");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = manager.poll(summary.process_id);
        if (!snapshot.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    const auto kill_start = std::chrono::steady_clock::now();
    const auto snapshot = manager.kill(summary.process_id);
    const auto elapsed = std::chrono::steady_clock::now() - kill_start;

    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exit_code, 0);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

TEST(SubprocessTest, BackgroundManagerShutdownDoesNotDeadlockAfterCompletedProcess) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    const auto start = std::chrono::steady_clock::now();

    {
        BackgroundProcessManager manager;
        const auto summary = manager.start(
            {
                .command = "python3 -c \"import time; print('done', flush=True); time.sleep(0.1)\"",
                .use_shell = true,
            },
            "python3 shutdown test");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = manager.poll(summary.process_id);
            if (!snapshot.running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count(), 2);
}
