#include <catch2/catch_test_macros.hpp>
#include "coordinator/coordinator-manager.hpp"

#include <atomic>
#include <chrono>
#include <thread>

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

    for (int i = 0; i < 50 && !saw_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(saw_stop.load());

    const auto run = manager.get_run(result.run_id);
    REQUIRE(run.has_value());
    CHECK((run->status == orangutan::coordinator::agent_run_status::terminated || run->status == orangutan::coordinator::agent_run_status::succeeded));

    manager.shutdown();
}

TEST_CASE("CoordinatorManager routes task notifications to parent runtime handler", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(1);
    std::string delivered;

    manager.register_runtime_notification_handler("parent-runtime", [&delivered](const std::string &message) -> std::optional<std::string> {
        delivered = message;
        return std::nullopt;
    });

    manager.set_worker_runtime_factory([](const orangutan::coordinator::AgentSpawnRequest &) {
        struct ImmediateWorker final : orangutan::coordinator::WorkerRuntime {
            std::string run(const std::string &, std::stop_token) override {
                return "done <ok> & more";
            }
        };
        return std::make_unique<ImmediateWorker>();
    });

    const auto result = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "notify parent <phase>",
        .parent_runtime_key = "parent-runtime",
    });
    REQUIRE(result.accepted);

    for (int i = 0; i < 50 && delivered.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(delivered.contains("<task-notification>"));
    CHECK(delivered.contains(result.run_id));
    CHECK(delivered.contains("notify parent &lt;phase&gt;"));
    CHECK(delivered.contains("done &lt;ok&gt; &amp; more"));
    CHECK_FALSE(delivered.contains("<result>done <ok> & more</result>"));

    manager.shutdown();
}

TEST_CASE("CoordinatorManager escapes quotes in task notifications", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(1);
    std::string delivered;

    manager.register_runtime_notification_handler("parent-runtime", [&delivered](const std::string &message) -> std::optional<std::string> {
        delivered = message;
        return std::nullopt;
    });

    manager.set_worker_runtime_factory([](const orangutan::coordinator::AgentSpawnRequest &) {
        struct ImmediateWorker final : orangutan::coordinator::WorkerRuntime {
            std::string run(const std::string &, std::stop_token) override {
                return "done \"quoted\" & 'single'";
            }
        };
        return std::make_unique<ImmediateWorker>();
    });

    const auto result = manager.spawn({
        .agent_key = "general-purpose",
        .task_prompt = "notify \"quotes\" and 'apostrophes'",
        .parent_runtime_key = "parent-runtime",
    });
    REQUIRE(result.accepted);

    for (int i = 0; i < 50 && delivered.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(delivered.contains("notify &quot;quotes&quot; and &apos;apostrophes&apos;"));
    CHECK(delivered.contains("done &quot;quoted&quot; &amp; &apos;single&apos;"));
    CHECK_FALSE(delivered.contains("notify \"quotes\" and 'apostrophes'"));
    CHECK_FALSE(delivered.contains("done \"quoted\" & 'single'"));

    manager.shutdown();
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

    for (int i = 0; i < 50 && started.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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

    for (int i = 0; i < 100; ++i) {
        const auto run = manager.get_run(second.run_id);
        if (run.has_value() && run->status == orangutan::coordinator::agent_run_status::succeeded) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

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

    for (int i = 0; i < 100 && !first_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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
