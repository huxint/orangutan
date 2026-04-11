#include <catch2/catch_test_macros.hpp>
#include "coordinator/coordinator-manager.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <string>
#include <thread>

namespace {

    template <typename Predicate>
    bool wait_for_condition(Predicate &&predicate, int attempts, std::chrono::milliseconds interval = std::chrono::milliseconds(10)) {
        for (int i = 0; i < attempts; ++i) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(interval);
        }
        return predicate();
    }

} // namespace

TEST_CASE("CoordinatorManager basic lifecycle", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(2);

    SECTION("starts empty") {
        REQUIRE(manager.list_active_runs().empty());
    }

    SECTION("shutdown is safe when empty") {
        manager.shutdown();
        REQUIRE(manager.list_active_runs().empty());
    }
}

TEST_CASE("CoordinatorManager stop requests the worker stop token", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(1);
    std::atomic<bool> saw_stop{false};

    manager.set_worker_runtime_factory([&saw_stop](const orangutan::coordinator::AgentSpawnRequest &) {
        struct SlowWorker final : orangutan::coordinator::WorkerRuntime {
            std::atomic<bool> *saw_stop = nullptr;

            std::string run(const std::string &, std::stop_token stop_token) override {
                for (int i = 0; i < 40; ++i) {
                    if (stop_token.stop_requested()) {
                        saw_stop->store(true);
                        return "stopped";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return "completed";
            }
        };

        auto worker = std::make_unique<SlowWorker>();
        worker->saw_stop = &saw_stop;
        return worker;
    });

    const auto result = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "slow task",
        .parent_runtime_key = "test-runtime",
    });
    REQUIRE(result.accepted);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    manager.stop(result.run_id);

    CHECK(wait_for_condition(
        [&] {
            return saw_stop.load();
        },
        50));
    CHECK(saw_stop.load());

    const auto run = manager.get_run(result.run_id);
    REQUIRE(run.has_value());
    CHECK((run->status == orangutan::coordinator::agent_run_status::terminated || run->status == orangutan::coordinator::agent_run_status::succeeded));

    manager.shutdown();
}

TEST_CASE("CoordinatorManager escapes xml-sensitive notification content", "[coordinator]") {
    struct EscapeCase {
        std::string task_prompt;
        std::string worker_output;
        std::string expected_escaped_prompt;
        std::string expected_escaped_result;
        std::string unescaped_prompt;
        std::string unescaped_result;
    };

    const std::array<EscapeCase, 2> cases{{
        {
            .task_prompt = "notify parent <phase>",
            .worker_output = "done <ok> & more",
            .expected_escaped_prompt = "notify parent &lt;phase&gt;",
            .expected_escaped_result = "done &lt;ok&gt; &amp; more",
            .unescaped_prompt = "notify parent <phase>",
            .unescaped_result = "done <ok> & more",
        },
        {
            .task_prompt = "notify \"quotes\" and 'apostrophes'",
            .worker_output = "done \"quoted\" & 'single'",
            .expected_escaped_prompt = "notify &quot;quotes&quot; and &apos;apostrophes&apos;",
            .expected_escaped_result = "done &quot;quoted&quot; &amp; &apos;single&apos;",
            .unescaped_prompt = "notify \"quotes\" and 'apostrophes'",
            .unescaped_result = "done \"quoted\" & 'single'",
        },
    }};

    for (const auto &test_case : cases) {
        orangutan::coordinator::CoordinatorManager manager(1);
        std::string delivered;

        manager.register_runtime_notification_handler("parent-runtime", [&delivered](const std::string &message) -> std::optional<std::string> {
            delivered = message;
            return std::nullopt;
        });

        manager.set_worker_runtime_factory([worker_output = test_case.worker_output](const orangutan::coordinator::AgentSpawnRequest &) {
            struct ImmediateWorker final : orangutan::coordinator::WorkerRuntime {
                std::string result;

                std::string run(const std::string &, std::stop_token) override {
                    return result;
                }
            };

            auto worker = std::make_unique<ImmediateWorker>();
            worker->result = worker_output;
            return worker;
        });

        const auto result = manager.spawn({
            .agent_key = "general-purpose",
            .task_prompt = test_case.task_prompt,
            .parent_runtime_key = "parent-runtime",
        });
        INFO("task_prompt=" << test_case.task_prompt);
        REQUIRE(result.accepted);

        CHECK(wait_for_condition(
            [&] {
                return !delivered.empty();
            },
            50));

        CHECK(delivered.contains("<task-notification>"));
        CHECK(delivered.contains(result.run_id));
        CHECK(delivered.contains(test_case.expected_escaped_prompt));
        CHECK(delivered.contains(test_case.expected_escaped_result));
        CHECK_FALSE(delivered.contains(test_case.unescaped_prompt));
        CHECK_FALSE(delivered.contains(test_case.unescaped_result));

        manager.shutdown();
    }
}

TEST_CASE("CoordinatorManager queues runs beyond max concurrency and starts them later", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(1);
    std::atomic<bool> release_first{false};
    std::atomic<int> started{0};

    manager.set_worker_runtime_factory([&](const orangutan::coordinator::AgentSpawnRequest &request) {
        struct QueuedWorker final : orangutan::coordinator::WorkerRuntime {
            std::atomic<bool> *release_first = nullptr;
            std::atomic<int> *started = nullptr;
            std::string task_prompt;

            std::string run(const std::string &, std::stop_token) override {
                started->fetch_add(1);
                if (task_prompt == "first task") {
                    for (int i = 0; i < 100 && !release_first->load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                return task_prompt + " done";
            }
        };

        auto worker = std::make_unique<QueuedWorker>();
        worker->release_first = &release_first;
        worker->started = &started;
        worker->task_prompt = request.task_prompt;
        return worker;
    });

    const auto first = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "first task",
        .parent_runtime_key = "test-runtime",
    });
    REQUIRE(first.accepted);

    REQUIRE(wait_for_condition(
        [&] {
            return started.load() >= 1;
        },
        50));
    REQUIRE(started.load() == 1);

    const auto second = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "second task",
        .parent_runtime_key = "test-runtime",
    });
    REQUIRE(second.accepted);

    const auto queued_run = manager.get_run(second.run_id);
    REQUIRE(queued_run.has_value());
    CHECK(queued_run->status == orangutan::coordinator::agent_run_status::queued);
    CHECK(started.load() == 1);

    release_first.store(true);

    CHECK(wait_for_condition(
        [&] {
            const auto run = manager.get_run(second.run_id);
            return run.has_value() && run->status == orangutan::coordinator::agent_run_status::succeeded;
        },
        100));

    CHECK(started.load() == 2);
    const auto completed_run = manager.get_run(second.run_id);
    REQUIRE(completed_run.has_value());
    CHECK(completed_run->status == orangutan::coordinator::agent_run_status::succeeded);
    CHECK(completed_run->final_output == "second task done");

    manager.shutdown();
}

TEST_CASE("CoordinatorManager shutdown does not block on queued runs", "[coordinator]") {
    using namespace std::chrono_literals;

    orangutan::coordinator::CoordinatorManager manager(1);
    std::atomic<bool> first_started{false};

    manager.set_worker_runtime_factory([&](const orangutan::coordinator::AgentSpawnRequest &request) {
        struct BlockingWorker final : orangutan::coordinator::WorkerRuntime {
            std::atomic<bool> *first_started = nullptr;
            std::string prompt;

            std::string run(const std::string &, std::stop_token stop_token) override {
                if (prompt == "first") {
                    first_started->store(true);
                }
                while (!stop_token.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return "stopped";
            }
        };

        auto worker = std::make_unique<BlockingWorker>();
        worker->first_started = &first_started;
        worker->prompt = request.task_prompt;
        return worker;
    });

    const auto first = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "first",
    });
    REQUIRE(first.accepted);

    REQUIRE(wait_for_condition(
        [&] {
            return first_started.load();
        },
        100));
    REQUIRE(first_started.load());

    const auto second = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "second",
    });
    REQUIRE(second.accepted);

    const auto queued = manager.get_run(second.run_id);
    REQUIRE(queued.has_value());
    CHECK(queued->status == orangutan::coordinator::agent_run_status::queued);

    const auto started_at = std::chrono::steady_clock::now();
    manager.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    CHECK(elapsed < 2s);
}
