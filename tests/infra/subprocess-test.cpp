#include "infra/subprocess/subprocess.hpp"
#include "test-helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;
using orangutan::testing::test_tmp_root;

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

TEST(SubprocessTest, BackgroundManagerPublishesCompletionEventExactlyOnceWithBoundedOutput) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    constexpr size_t max_output_tail_bytes = 16384;
    const std::string stdout_text = std::string(20000, 'O') + "stdout-tail\n";
    const std::string stderr_text = std::string(19000, 'E') + "stderr-tail\n";

    std::mutex mutex;
    std::condition_variable cv;
    size_t callback_count = 0;
    std::optional<BackgroundProcessCompletionEvent> completion_event;

    BackgroundProcessManager manager([&](const BackgroundProcessCompletionEvent &event) {
        std::scoped_lock lock(mutex);
        ++callback_count;
        completion_event = event;
        cv.notify_all();
    });

    const auto summary = manager.start(
        {
            .command = "python3 -c \"import sys; sys.stdout.write('O'*20000 + 'stdout-tail\\n'); "
                       "sys.stderr.write('E'*19000 + 'stderr-tail\\n')\"",
            .use_shell = true,
        },
        "completion event test",
        {
            .publish_completion_event = true,
            .metadata = {{"job", "completion-test"}, {"token", "abc123"}},
        });

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&completion_event] {
            return completion_event.has_value();
        }));
    }

    const auto snapshot = manager.poll(summary.process_id);
    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exit_code, 0);

    const auto kill_snapshot = manager.kill(summary.process_id);
    EXPECT_FALSE(kill_snapshot.running);
    EXPECT_EQ(kill_snapshot.exit_code, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::scoped_lock lock(mutex);
    ASSERT_EQ(callback_count, 1U);
    ASSERT_TRUE(completion_event.has_value());

    EXPECT_EQ(completion_event->process_id, summary.process_id);
    EXPECT_EQ(completion_event->command, "completion event test");
    EXPECT_EQ(completion_event->pid, summary.pid);
    EXPECT_FALSE(completion_event->kill_requested);
    EXPECT_EQ(completion_event->terminal_status, BackgroundProcessTerminalStatus::exited);
    ASSERT_TRUE(completion_event->exit_code.has_value());
    EXPECT_EQ(*completion_event->exit_code, 0);
    EXPECT_FALSE(completion_event->signal_number.has_value());
    EXPECT_EQ(completion_event->metadata.at("job"), "completion-test");
    EXPECT_EQ(completion_event->metadata.at("token"), "abc123");

    EXPECT_EQ(completion_event->stdout.total_bytes, stdout_text.size());
    EXPECT_TRUE(completion_event->stdout.truncated);
    EXPECT_EQ(completion_event->stdout.tail.size(), max_output_tail_bytes);
    EXPECT_EQ(completion_event->stdout.tail, stdout_text.substr(stdout_text.size() - max_output_tail_bytes));

    EXPECT_EQ(completion_event->stderr.total_bytes, stderr_text.size());
    EXPECT_TRUE(completion_event->stderr.truncated);
    EXPECT_EQ(completion_event->stderr.tail.size(), max_output_tail_bytes);
    EXPECT_EQ(completion_event->stderr.tail, stderr_text.substr(stderr_text.size() - max_output_tail_bytes));
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

TEST(SubprocessTest, BackgroundCompletionCallbackCanReenterManagerKillOnSignal) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    std::mutex mutex;
    std::condition_variable cv;
    size_t callback_count = 0;
    std::optional<BackgroundProcessCompletionEvent> completion_event;
    std::optional<BackgroundProcessSnapshot> callback_snapshot;
    BackgroundProcessManager *manager_ptr = nullptr;

    BackgroundProcessManager manager([&](const BackgroundProcessCompletionEvent &event) {
        auto snapshot = manager_ptr->kill(event.process_id);
        std::scoped_lock lock(mutex);
        ++callback_count;
        completion_event = event;
        callback_snapshot = std::move(snapshot);
        cv.notify_all();
    });
    manager_ptr = &manager;

    const auto summary = manager.start(
        {
            .command = "python3 -c \"import time; time.sleep(10)\"",
            .use_shell = true,
        },
        "reentrant callback test",
        {
            .publish_completion_event = true,
            .metadata = {{"mode", "signal"}},
        });

    const auto kill_snapshot = manager.kill(summary.process_id);
    EXPECT_FALSE(kill_snapshot.running);
    EXPECT_TRUE(kill_snapshot.kill_requested);
    ASSERT_TRUE(kill_snapshot.signal_number.has_value());

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return completion_event.has_value() && callback_snapshot.has_value();
        }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::scoped_lock lock(mutex);
    ASSERT_EQ(callback_count, 1U);
    ASSERT_TRUE(completion_event.has_value());
    ASSERT_TRUE(callback_snapshot.has_value());

    EXPECT_EQ(completion_event->process_id, summary.process_id);
    EXPECT_TRUE(completion_event->kill_requested);
    EXPECT_EQ(completion_event->terminal_status, BackgroundProcessTerminalStatus::signaled);
    ASSERT_TRUE(completion_event->signal_number.has_value());
    EXPECT_EQ(completion_event->signal_number, kill_snapshot.signal_number);
    ASSERT_TRUE(completion_event->exit_code.has_value());
    EXPECT_EQ(completion_event->metadata.at("mode"), "signal");

    EXPECT_EQ(callback_snapshot->process_id, summary.process_id);
    EXPECT_FALSE(callback_snapshot->running);
    EXPECT_TRUE(callback_snapshot->kill_requested);
    EXPECT_EQ(callback_snapshot->signal_number, completion_event->signal_number);
}

TEST(SubprocessTest, BackgroundCompletionCallbackExceptionsAreSwallowed) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    std::atomic<int> callback_count{0};

    BackgroundProcessManager manager([&](const BackgroundProcessCompletionEvent &) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("boom");
    });

    const auto summary = manager.start(
        {
            .command = "printf 'done\\n'",
            .use_shell = true,
        },
        "throwing callback test",
        {
            .publish_completion_event = true,
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    BackgroundProcessSnapshot snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = manager.poll(summary.process_id);
        if (!snapshot.running && callback_count.load(std::memory_order_relaxed) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exit_code, 0);
    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1);
}

TEST(SubprocessTest, BackgroundManagerRejectsReentrantStartDuringShutdown) {
    EXPECT_EXIT(
        {
            ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
            std::atomic<bool> shutdown_started{false};
            std::atomic<bool> callback_attempted{false};
            std::atomic<bool> start_rejected{false};

            BackgroundProcessManager *manager_ptr = nullptr;
            auto manager = std::make_unique<BackgroundProcessManager>([&](const BackgroundProcessCompletionEvent &) {
                if (!shutdown_started.load(std::memory_order_acquire)) {
                    return;
                }

                callback_attempted.store(true, std::memory_order_release);
                try {
                    (void)manager_ptr->start(
                        {
                            .command = "/bin/true",
                            .use_shell = false,
                        },
                        "shutdown reentrant start");
                } catch (const std::exception &) {
                    start_rejected.store(true, std::memory_order_release);
                }
            });
            manager_ptr = manager.get();

            (void)manager->start(
                {
                    .command = "python3 -c \"import time; time.sleep(10)\"",
                    .use_shell = true,
                },
                "shutdown gate test",
                {
                    .publish_completion_event = true,
                });

            shutdown_started.store(true, std::memory_order_release);
            manager.reset();

            if (!callback_attempted.load(std::memory_order_acquire)) {
                std::exit(2);
            }
            if (!start_rejected.load(std::memory_order_acquire)) {
                std::exit(3);
            }
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(SubprocessTest, BackgroundManagerReclaimsWaitThreadsAcrossManyCompletedProcesses) {
    ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
    std::atomic<int> callback_count{0};
    BackgroundProcessManager manager([&](const BackgroundProcessCompletionEvent &) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    constexpr int iterations = 512;
    for (int i = 0; i < iterations; ++i) {
        const auto summary = manager.start(
            {
                .command = "/bin/true",
                .use_shell = false,
            },
            "thread cleanup test",
            {
                .publish_completion_event = true,
            });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        BackgroundProcessSnapshot snapshot;
        while (std::chrono::steady_clock::now() < deadline) {
            snapshot = manager.poll(summary.process_id);
            if (!snapshot.running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        ASSERT_FALSE(snapshot.running) << "iteration " << i;
        EXPECT_EQ(snapshot.exit_code, 0) << "iteration " << i;

        const auto final_snapshot = manager.kill(summary.process_id);
        EXPECT_FALSE(final_snapshot.running) << "iteration " << i;
        EXPECT_EQ(final_snapshot.exit_code, 0) << "iteration " << i;
    }

    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), iterations);
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
