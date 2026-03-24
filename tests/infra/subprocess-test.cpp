#include "infra/subprocess/subprocess.hpp"
#include "infra/execution/sender-utils.hpp"
#include "test-helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include "support/ut.hpp"
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

boost::ut::suite subprocess_suite = [] {
    using namespace boost::ut;

    // ── Basic execution ─────────────────────────────

    "echo_command"_test = [] {
        auto result = run_subprocess({.command = "echo hello"});
        expect(result.exit_code == 0);
        expect(result.stdout_output == "hello\n");
        expect(result.stderr_output.empty());
        expect(not result.timed_out);
    };

    "captures_stdout"_test = [] {
        auto result = run_subprocess({.command = "printf 'line1\nline2\n'"});
        expect(result.exit_code == 0);
        expect(result.stdout_output == "line1\nline2\n");
    };

    "captures_stderr"_test = [] {
        auto result = run_subprocess({.command = "echo err >&2"});
        expect(result.exit_code == 0);
        expect(result.stderr_output == "err\n");
    };

    "stdin_delivery"_test = [] {
        auto result = run_subprocess({.command = "cat", .stdin_data = "hello from stdin"});
        expect(result.exit_code == 0);
        expect(result.stdout_output == "hello from stdin");
    };

    "sender_primitive_executes_subprocess"_test = [] {
        auto pipeline = run_subprocess_sender({.command = "printf 'sender-path\\n'"});
        auto [result] = execution::sync_wait_or_throw(std::move(pipeline), "subprocess sender test");
        expect(result.exit_code == 0);
        expect(result.stdout_output == "sender-path\n");
        expect(result.stderr_output.empty());
        expect(not result.timed_out);
    };

    "non_zero_exit_code"_test = [] {
        auto result = run_subprocess({.command = "exit 42"});
        expect(result.exit_code == 42);
        expect(not result.timed_out);
    };

    "command_not_found"_test = [] {
        auto result = run_subprocess({.command = "nonexistent_command_xyz_12345"});
        expect(result.exit_code == 127);
    };

    // ── Timeout enforcement ─────────────────────────

    "timeout_kills_quiet_child"_test = [] {
        auto start = std::chrono::steady_clock::now();
        auto result = run_subprocess({.command = "sleep 999", .timeout = std::chrono::seconds(1)});
        auto elapsed = std::chrono::steady_clock::now() - start;

        expect(result.timed_out);
        expect(result.exit_code == -1);
        // Should complete in ~1s, not 999s
        expect(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
    };

    "timeout_still_applies_after_child_closes_output"_test = [] {
        auto start = std::chrono::steady_clock::now();
        auto result = run_subprocess({.command = "exec >/dev/null 2>&1; sleep 999", .timeout = std::chrono::seconds(1)});
        auto elapsed = std::chrono::steady_clock::now() - start;

        expect(result.timed_out);
        expect(result.exit_code == -1);
        expect(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
    };

    // ── Working directory ───────────────────────────

    "working_directory"_test = [] {
        const auto working_dir = test_tmp_root() / "subprocess-working-dir";
        std::filesystem::remove_all(working_dir);
        std::filesystem::create_directories(working_dir);

        auto result = run_subprocess({.command = "pwd", .working_dir = working_dir.string()});
        expect(result.exit_code == 0);
        expect(result.stdout_output == working_dir.string() + "\n");

        std::filesystem::remove_all(working_dir);
    };

    // ── Pipe deadlock regression ────────────────────

    "large_stderr_does_not_deadlock"_test = [] {
        // Child writes > 64KB to stderr while also writing to stdout.
        // Without poll()-based multiplexing, this would deadlock.
        auto result = run_subprocess({
            .command = "python3 -c \"import sys; sys.stderr.write('E'*100000); sys.stdout.write('O'*100000)\"",
            .timeout = std::chrono::seconds(10),
        });

        // If we get here, no deadlock occurred
        expect(result.exit_code == 0);
        expect(not result.timed_out);
        expect(result.stdout_output.size() == 100000);
        expect(result.stderr_output.size() == 100000);
    };

    "large_stdin_and_stdout_do_not_deadlock"_test = [] {
        std::string input(100000, 'I');
        auto result = run_subprocess({
            .command = "python3 -c \"import sys; sys.stdout.write('O'*100000); sys.stdout.flush(); data=sys.stdin.read(); sys.stderr.write(str(len(data)))\"",
            .stdin_data = input,
            .timeout = std::chrono::seconds(10),
        });

        expect(result.exit_code == 0);
        expect(not result.timed_out);
        expect(result.stdout_output.size() == 100000);
        expect(result.stderr_output == "100000");
    };

    "background_manager_captures_output_and_exit"_test = [] {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        BackgroundProcessManager manager;
        const auto summary = manager.start(
            {
                .command = "python3 -c \"import time; print('tick', flush=True); time.sleep(0.2); print('tock', flush=True)\"",
                .use_shell = true,
            },
            "python3 background test");

        expect(not summary.process_id.empty());
        expect(summary.running);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        BackgroundProcessSnapshot snapshot;
        while (std::chrono::steady_clock::now() < deadline) {
            snapshot = manager.poll(summary.process_id);
            if (!snapshot.running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        expect(not snapshot.running);
        expect(snapshot.exit_code == 0);
        expect(snapshot.stdout_output.find("tick") != std::string::npos);
        expect(snapshot.stdout_output.find("tock") != std::string::npos);
    };

    "background_manager_publishes_completion_event_exactly_once_with_bounded_output"_test = [] {
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
            expect(cv.wait_for(lock, std::chrono::seconds(5), [&completion_event] {
                return completion_event.has_value();
            }) >> fatal);
        }

        const auto snapshot = manager.poll(summary.process_id);
        expect(not snapshot.running);
        expect(snapshot.exit_code == 0);

        const auto kill_snapshot = manager.kill(summary.process_id);
        expect(not kill_snapshot.running);
        expect(kill_snapshot.exit_code == 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::scoped_lock lock(mutex);
        expect((callback_count == 1U) >> fatal);
        expect(completion_event.has_value() >> fatal);

        expect(completion_event->process_id == summary.process_id);
        expect(completion_event->command == "completion event test");
        expect(completion_event->pid == summary.pid);
        expect(not completion_event->kill_requested);
        expect(completion_event->terminal_status == BackgroundProcessTerminalStatus::exited);
        expect(completion_event->exit_code.has_value() >> fatal);
        expect(*completion_event->exit_code == 0);
        expect(not completion_event->signal_number.has_value());
        expect(completion_event->metadata.at("job") == "completion-test");
        expect(completion_event->metadata.at("token") == "abc123");

        expect(completion_event->stdout.total_bytes == stdout_text.size());
        expect(completion_event->stdout.truncated);
        expect(completion_event->stdout.tail.size() == max_output_tail_bytes);
        expect(completion_event->stdout.tail == stdout_text.substr(stdout_text.size() - max_output_tail_bytes));

        expect(completion_event->stderr.total_bytes == stderr_text.size());
        expect(completion_event->stderr.truncated);
        expect(completion_event->stderr.tail.size() == max_output_tail_bytes);
        expect(completion_event->stderr.tail == stderr_text.substr(stderr_text.size() - max_output_tail_bytes));
    };

    "background_manager_kill_stops_running_process"_test = [] {
        ScopedEnvVar tmp_env("TMPDIR", test_tmp_root().string());
        BackgroundProcessManager manager;
        const auto summary = manager.start(
            {
                .command = "python3 -c \"import time; time.sleep(10)\"",
                .use_shell = true,
            },
            "python3 kill test");

        const auto snapshot = manager.kill(summary.process_id);
        expect(not snapshot.running);
        expect(snapshot.kill_requested);
        expect(snapshot.signal_number.has_value());
    };

    "background_completion_callback_can_reenter_manager_kill_on_signal"_test = [] {
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
        expect(not kill_snapshot.running);
        expect(kill_snapshot.kill_requested);
        expect(kill_snapshot.signal_number.has_value() >> fatal);

        {
            std::unique_lock lock(mutex);
            expect(cv.wait_for(lock, std::chrono::seconds(5), [&] {
                return completion_event.has_value() && callback_snapshot.has_value();
            }) >> fatal);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::scoped_lock lock(mutex);
        expect((callback_count == 1U) >> fatal);
        expect(completion_event.has_value() >> fatal);
        expect(callback_snapshot.has_value() >> fatal);

        expect(completion_event->process_id == summary.process_id);
        expect(completion_event->kill_requested);
        expect(completion_event->terminal_status == BackgroundProcessTerminalStatus::signaled);
        expect(completion_event->signal_number.has_value() >> fatal);
        expect(completion_event->signal_number == kill_snapshot.signal_number);
        expect(completion_event->exit_code.has_value() >> fatal);
        expect(completion_event->metadata.at("mode") == "signal");

        expect(callback_snapshot->process_id == summary.process_id);
        expect(not callback_snapshot->running);
        expect(callback_snapshot->kill_requested);
        expect(callback_snapshot->signal_number == completion_event->signal_number);
    };

    "background_completion_callback_exceptions_are_swallowed"_test = [] {
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

        expect(not snapshot.running);
        expect(snapshot.exit_code == 0);
        expect(callback_count.load(std::memory_order_relaxed) == 1);
    };

    "background_manager_rejects_reentrant_start_during_shutdown"_test = [] {
        const auto child = ::fork();
        expect((child != -1) >> fatal);
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
        expect((::waitpid(child, &status, 0) == child) >> fatal);
        expect(WIFEXITED(status) >> fatal);
        expect(WEXITSTATUS(status) == 0);
    };

    "background_manager_reclaims_wait_threads_across_many_completed_processes"_test = [] {
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

            expect((not snapshot.running) >> fatal) << "iteration " << i;
            expect(snapshot.exit_code == 0) << "iteration " << i;

            const auto final_snapshot = manager.kill(summary.process_id);
            expect(not final_snapshot.running) << "iteration " << i;
            expect(final_snapshot.exit_code == 0) << "iteration " << i;
        }

        expect(callback_count.load(std::memory_order_relaxed) == iterations);
    };

    "background_manager_kill_after_completion_does_not_deadlock"_test = [] {
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

        expect(not snapshot.running);
        expect(snapshot.exit_code == 0);
        expect(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);
    };

    "background_manager_shutdown_does_not_deadlock_after_completed_process"_test = [] {
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

        expect(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < 2);
    };
};

} // namespace
