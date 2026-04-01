#include "process/subprocess.hpp"
#include "utils/sender-utils.hpp"
#include "test-helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;
using orangutan::testing::test_tmp_root;

namespace {

    // ── Basic execution ─────────────────────────────

    TEST_CASE("echo_command") {
        auto result = run_subprocess({.command = "echo hello"});
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "hello\n");
        CHECK(result.stderr_output.empty());
        CHECK_FALSE(result.timed_out);
    };

    TEST_CASE("captures_stdout") {
        auto result = run_subprocess({.command = "printf 'line1\nline2\n'"});
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "line1\nline2\n");
    };

    TEST_CASE("captures_stderr") {
        auto result = run_subprocess({.command = "echo err >&2"});
        CHECK(result.exit_code == 0);
        CHECK(result.stderr_output == "err\n");
    };

    TEST_CASE("stdin_delivery") {
        auto result = run_subprocess({.command = "cat", .stdin_data = "hello from stdin"});
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "hello from stdin");
    };

    TEST_CASE("sender_primitive_executes_subprocess") {
        auto pipeline = run_subprocess_sender({.command = "printf 'sender-path\\n'"});
        auto [result] = execution::sync_wait_or_throw(std::move(pipeline), "subprocess sender test");
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "sender-path\n");
        CHECK(result.stderr_output.empty());
        CHECK_FALSE(result.timed_out);
    };

    TEST_CASE("non_zero_exit_code") {
        auto result = run_subprocess({.command = "exit 42"});
        CHECK(result.exit_code == 42);
        CHECK_FALSE(result.timed_out);
    };

    TEST_CASE("command_not_found") {
        auto result = run_subprocess({.command = "nonexistent_command_xyz_12345"});
        CHECK(result.exit_code == 127);
    };

    // ── Timeout enforcement ─────────────────────────

    TEST_CASE("timeout_kills_quiet_child") {
        auto start = std::chrono::steady_clock::now();
        auto result = run_subprocess({.command = "sleep 999", .timeout = std::chrono::seconds(1)});
        auto elapsed = std::chrono::steady_clock::now() - start;

        CHECK(result.timed_out);
        CHECK(result.exit_code == -1);
        // Should complete in ~1s, not 999s
        CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
    };

    TEST_CASE("timeout_still_applies_after_child_closes_output") {
        auto start = std::chrono::steady_clock::now();
        auto result = run_subprocess({.command = "exec >/dev/null 2>&1; sleep 999", .timeout = std::chrono::seconds(1)});
        auto elapsed = std::chrono::steady_clock::now() - start;

        CHECK(result.timed_out);
        CHECK(result.exit_code == -1);
        CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
    };

    // ── Working directory ───────────────────────────

    TEST_CASE("working_directory") {
        const auto working_dir = test_tmp_root() / "subprocess-working-dir";
        std::filesystem::remove_all(working_dir);
        std::filesystem::create_directories(working_dir);

        auto result = run_subprocess({.command = "pwd", .working_dir = working_dir.string()});
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == working_dir.string() + "\n");

        std::filesystem::remove_all(working_dir);
    };

    // ── Pipe deadlock regression ────────────────────

    TEST_CASE("large_stderr_does_not_deadlock") {
        // Child writes > 64KB to stderr while also writing to stdout.
        // Without poll()-based multiplexing, this would deadlock.
        auto result = run_subprocess({
            .command = "python3 -c \"import sys; sys.stderr.write('E'*100000); sys.stdout.write('O'*100000)\"",
            .timeout = std::chrono::seconds(10),
        });

        // If we get here, no deadlock occurred
        CHECK(result.exit_code == 0);
        CHECK_FALSE(result.timed_out);
        CHECK(result.stdout_output.size() == 100000);
        CHECK(result.stderr_output.size() == 100000);
    };

    TEST_CASE("large_stdin_and_stdout_do_not_deadlock") {
        std::string input(100000, 'I');
        auto result = run_subprocess({
            .command = "python3 -c \"import sys; sys.stdout.write('O'*100000); sys.stdout.flush(); data=sys.stdin.read(); sys.stderr.write(str(len(data)))\"",
            .stdin_data = input,
            .timeout = std::chrono::seconds(10),
        });

        CHECK(result.exit_code == 0);
        CHECK_FALSE(result.timed_out);
        CHECK(result.stdout_output.size() == 100000);
        CHECK(result.stderr_output == "100000");
    };

    TEST_CASE("background_manager_captures_output_and_exit") {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        BackgroundProcessManager manager;
        const auto summary = manager.start(
            {
                .command = "python3 -c \"import time; print('tick', flush=True); time.sleep(0.2); print('tock', flush=True)\"",
                .use_shell = true,
            },
            "python3 background test");

        CHECK_FALSE(summary.process_id.empty());
        CHECK(summary.running);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        BackgroundProcessSnapshot snapshot;
        while (std::chrono::steady_clock::now() < deadline) {
            snapshot = manager.poll(summary.process_id);
            if (!snapshot.running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        CHECK_FALSE(snapshot.running);
        CHECK(snapshot.exit_code == 0);
        CHECK(snapshot.stdout_output.contains("tick"));
        CHECK(snapshot.stdout_output.contains("tock"));
    };

    TEST_CASE("background_manager_publishes_completion_event_exactly_once_with_bounded_output") {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        constexpr std::size_t max_output_tail_bytes = 16384;
        const std::string stdout_text = std::string(20000, 'O') + "stdout-tail\n";
        const std::string stderr_text = std::string(19000, 'E') + "stderr-tail\n";

        std::mutex mutex;
        std::condition_variable cv;
        std::size_t callback_count = 0;
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
            REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&completion_event] {
                return completion_event.has_value();
            }));
        }

        const auto snapshot = manager.poll(summary.process_id);
        CHECK_FALSE(snapshot.running);
        CHECK(snapshot.exit_code == 0);

        const auto kill_snapshot = manager.kill(summary.process_id);
        CHECK_FALSE(kill_snapshot.running);
        CHECK(kill_snapshot.exit_code == 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::scoped_lock lock(mutex);
        REQUIRE(callback_count == 1U);
        REQUIRE(completion_event.has_value());

        CHECK(completion_event->process_id == summary.process_id);
        CHECK(completion_event->command == "completion event test");
        CHECK(completion_event->pid == summary.pid);
        CHECK_FALSE(completion_event->kill_requested);
        CHECK(completion_event->terminal_status == BackgroundProcessTerminalStatus::exited);
        REQUIRE(completion_event->exit_code.has_value());
        CHECK(*completion_event->exit_code == 0);
        CHECK_FALSE(completion_event->signal_number.has_value());
        CHECK(completion_event->metadata.at("job") == "completion-test");
        CHECK(completion_event->metadata.at("token") == "abc123");

        CHECK(completion_event->stdout.total_bytes == stdout_text.size());
        CHECK(completion_event->stdout.truncated);
        CHECK(completion_event->stdout.tail.size() == max_output_tail_bytes);
        CHECK(completion_event->stdout.tail == stdout_text.substr(stdout_text.size() - max_output_tail_bytes));

        CHECK(completion_event->stderr.total_bytes == stderr_text.size());
        CHECK(completion_event->stderr.truncated);
        CHECK(completion_event->stderr.tail.size() == max_output_tail_bytes);
        CHECK(completion_event->stderr.tail == stderr_text.substr(stderr_text.size() - max_output_tail_bytes));
    };

    TEST_CASE("background_manager_kill_stops_running_process") {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        BackgroundProcessManager manager;
        const auto summary = manager.start(
            {
                .command = "python3 -c \"import time; time.sleep(10)\"",
                .use_shell = true,
            },
            "python3 kill test");

        const auto snapshot = manager.kill(summary.process_id);
        CHECK_FALSE(snapshot.running);
        CHECK(snapshot.kill_requested);
        CHECK(snapshot.signal_number.has_value());
    };

    TEST_CASE("background_completion_callback_can_reenter_manager_kill_on_signal") {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t callback_count = 0;
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
        CHECK_FALSE(kill_snapshot.running);
        CHECK(kill_snapshot.kill_requested);
        REQUIRE(kill_snapshot.signal_number.has_value());

        {
            std::unique_lock lock(mutex);
            REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
                return completion_event.has_value() && callback_snapshot.has_value();
            }));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::scoped_lock lock(mutex);
        REQUIRE(callback_count == 1U);
        REQUIRE(completion_event.has_value());
        REQUIRE(callback_snapshot.has_value());

        CHECK(completion_event->process_id == summary.process_id);
        CHECK(completion_event->kill_requested);
        CHECK(completion_event->terminal_status == BackgroundProcessTerminalStatus::signaled);
        REQUIRE(completion_event->signal_number.has_value());
        CHECK(completion_event->signal_number == kill_snapshot.signal_number);
        REQUIRE(completion_event->exit_code.has_value());
        CHECK(completion_event->metadata.at("mode") == "signal");

        CHECK(callback_snapshot->process_id == summary.process_id);
        CHECK_FALSE(callback_snapshot->running);
        CHECK(callback_snapshot->kill_requested);
        CHECK(callback_snapshot->signal_number == completion_event->signal_number);
    };

    TEST_CASE("background_completion_callback_exceptions_are_swallowed") {
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

        CHECK_FALSE(snapshot.running);
        CHECK(snapshot.exit_code == 0);
        CHECK(callback_count.load(std::memory_order_relaxed) == 1);
    };

    TEST_CASE("background_manager_rejects_reentrant_start_during_shutdown") {
        const auto child = ::fork();
        REQUIRE(child != -1);
        if (child == 0) {
            try {
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
                        static_cast<void>(manager_ptr->start(
                            {
                                .command = "/bin/true",
                                .use_shell = false,
                            },
                            "shutdown reentrant start"));
                    } catch (const std::exception &) {
                        start_rejected.store(true, std::memory_order_release);
                    }
                });
                manager_ptr = manager.get();

                static_cast<void>(manager->start(
                    {
                        .command = "python3 -c \"import time; time.sleep(10)\"",
                        .use_shell = true,
                    },
                    "shutdown gate test",
                    {
                        .publish_completion_event = true,
                    }));

                shutdown_started.store(true, std::memory_order_release);
                manager.reset();

                if (!callback_attempted.load(std::memory_order_acquire)) {
                    std::_Exit(2);
                }
                if (!start_rejected.load(std::memory_order_acquire)) {
                    std::_Exit(3);
                }
                std::_Exit(0);
            } catch (...) {
                std::_Exit(4);
            }
        }

        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    };

    TEST_CASE("background_manager_reclaims_wait_threads_across_many_completed_processes") {
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

            INFO("iteration " << i);
            REQUIRE(not snapshot.running);
            INFO("iteration " << i);
            CHECK(snapshot.exit_code == 0);

            const auto final_snapshot = manager.kill(summary.process_id);
            INFO("iteration " << i);
            CHECK_FALSE(final_snapshot.running);
            INFO("iteration " << i);
            CHECK(final_snapshot.exit_code == 0);
        }

        CHECK(callback_count.load(std::memory_order_relaxed) == iterations);
    };

    TEST_CASE("background_manager_kill_after_completion_does_not_deadlock") {
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

        CHECK_FALSE(snapshot.running);
        CHECK(snapshot.exit_code == 0);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);
    };

    TEST_CASE("background_manager_shutdown_does_not_deadlock_after_completed_process") {
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

        CHECK(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < 2);
    };

} // namespace
