#include <catch2/catch_test_macros.hpp>
#include "orchestration/orchestration-manager.hpp"

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

TEST_CASE("OrchestrationManager basic lifecycle", "[orchestration]") {
    orangutan::orchestration::OrchestrationManager manager(2);

    SECTION("starts empty") {
        REQUIRE(manager.list_active_runs().empty());
    }

    SECTION("shutdown is safe when empty") {
        manager.shutdown();
        REQUIRE(manager.list_active_runs().empty());
    }
}

TEST_CASE("OrchestrationManager stop requests the teammate stop token", "[orchestration]") {
    orangutan::orchestration::OrchestrationManager manager(1);
    std::atomic<bool> saw_stop{false};

    manager.set_teammate_runtime_factory([&saw_stop](const orangutan::orchestration::AgentSpawnRequest &) {
        struct SlowTeammate final : orangutan::orchestration::TeammateRuntime {
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

        auto teammate = std::make_unique<SlowTeammate>();
        teammate->saw_stop = &saw_stop;
        return teammate;
    });

    const auto result = manager.spawn({
        .name = "teammate",
        .task = "slow task",
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
    CHECK((run->status == orangutan::orchestration::run_status::terminated || run->status == orangutan::orchestration::run_status::succeeded));

    manager.shutdown();
}

TEST_CASE("OrchestrationManager escapes xml-sensitive notification content", "[orchestration]") {
    struct EscapeCase {
        std::string task;
        std::string teammate_output;
        std::string expected_escaped_prompt;
        std::string expected_escaped_result;
        std::string unescaped_prompt;
        std::string unescaped_result;
    };

    const std::array<EscapeCase, 2> cases{{
        {
            .task = "notify parent <phase>",
            .teammate_output = "done <ok> & more",
            .expected_escaped_prompt = "notify parent &lt;phase&gt;",
            .expected_escaped_result = "done &lt;ok&gt; &amp; more",
            .unescaped_prompt = "notify parent <phase>",
            .unescaped_result = "done <ok> & more",
        },
        {
            .task = "notify \"quotes\" and 'apostrophes'",
            .teammate_output = "done \"quoted\" & 'single'",
            .expected_escaped_prompt = "notify &quot;quotes&quot; and &apos;apostrophes&apos;",
            .expected_escaped_result = "done &quot;quoted&quot; &amp; &apos;single&apos;",
            .unescaped_prompt = "notify \"quotes\" and 'apostrophes'",
            .unescaped_result = "done \"quoted\" & 'single'",
        },
    }};

    for (const auto &test_case : cases) {
        orangutan::orchestration::OrchestrationManager manager(1);
        std::string delivered;

        manager.register_runtime_notification_handler("parent-runtime", [&delivered](const std::string &message) -> std::optional<std::string> {
            delivered = message;
            return std::nullopt;
        });

        manager.set_teammate_runtime_factory([teammate_output = test_case.teammate_output](const orangutan::orchestration::AgentSpawnRequest &) {
            struct ImmediateTeammate final : orangutan::orchestration::TeammateRuntime {
                std::string result;

                std::string run(const std::string &, std::stop_token) override {
                    return result;
                }
            };

            auto teammate = std::make_unique<ImmediateTeammate>();
            teammate->result = teammate_output;
            return teammate;
        });

        const auto result = manager.spawn({
            .name = "teammate",
            .task = test_case.task,
            .parent_runtime_key = "parent-runtime",
        });
        INFO("task=" << test_case.task);
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

TEST_CASE("OrchestrationManager queues runs beyond max concurrency and starts them later", "[orchestration]") {
    orangutan::orchestration::OrchestrationManager manager(1);
    std::atomic<bool> release_first{false};
    std::atomic<int> started{0};

    manager.set_teammate_runtime_factory([&](const orangutan::orchestration::AgentSpawnRequest &request) {
        struct QueuedTeammate final : orangutan::orchestration::TeammateRuntime {
            std::atomic<bool> *release_first = nullptr;
            std::atomic<int> *started = nullptr;
            std::string task;

            std::string run(const std::string &, std::stop_token) override {
                started->fetch_add(1);
                if (task == "first task") {
                    for (int i = 0; i < 100 && !release_first->load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                return task + " done";
            }
        };

        auto teammate = std::make_unique<QueuedTeammate>();
        teammate->release_first = &release_first;
        teammate->started = &started;
        teammate->task = request.task;
        return teammate;
    });

    const auto first = manager.spawn({
        .name = "teammate",
        .task = "first task",
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
        .name = "teammate",
        .task = "second task",
        .parent_runtime_key = "test-runtime",
    });
    REQUIRE(second.accepted);

    const auto queued_run = manager.get_run(second.run_id);
    REQUIRE(queued_run.has_value());
    CHECK(queued_run->status == orangutan::orchestration::run_status::queued);
    CHECK(started.load() == 1);

    release_first.store(true);

    CHECK(wait_for_condition(
        [&] {
            const auto run = manager.get_run(second.run_id);
            return run.has_value() && run->status == orangutan::orchestration::run_status::succeeded;
        },
        100));

    CHECK(started.load() == 2);
    const auto completed_run = manager.get_run(second.run_id);
    REQUIRE(completed_run.has_value());
    CHECK(completed_run->status == orangutan::orchestration::run_status::succeeded);
    CHECK(completed_run->final_output == "second task done");

    manager.shutdown();
}

TEST_CASE("OrchestrationManager shutdown does not block on queued runs", "[orchestration]") {
    using namespace std::chrono_literals;

    orangutan::orchestration::OrchestrationManager manager(1);
    std::atomic<bool> first_started{false};

    manager.set_teammate_runtime_factory([&](const orangutan::orchestration::AgentSpawnRequest &request) {
        struct BlockingTeammate final : orangutan::orchestration::TeammateRuntime {
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

        auto teammate = std::make_unique<BlockingTeammate>();
        teammate->first_started = &first_started;
        teammate->prompt = request.task;
        return teammate;
    });

    const auto first = manager.spawn({
        .name = "teammate",
        .task = "first",
    });
    REQUIRE(first.accepted);

    REQUIRE(wait_for_condition(
        [&] {
            return first_started.load();
        },
        100));
    REQUIRE(first_started.load());

    const auto second = manager.spawn({
        .name = "teammate",
        .task = "second",
    });
    REQUIRE(second.accepted);

    const auto queued = manager.get_run(second.run_id);
    REQUIRE(queued.has_value());
    CHECK(queued->status == orangutan::orchestration::run_status::queued);

    const auto started_at = std::chrono::steady_clock::now();
    manager.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    CHECK(elapsed < 2s);
}

TEST_CASE("OrchestrationManager shutdown is safe while completion callback is still running", "[orchestration]") {
    using namespace std::chrono_literals;

    orangutan::orchestration::OrchestrationManager manager(1);
    std::atomic<bool> callback_started{false};

    manager.register_runtime_notification_handler("parent-runtime", [&callback_started](const std::string &) -> std::optional<std::string> {
        callback_started.store(true);
        std::this_thread::sleep_for(300ms);
        return std::nullopt;
    });

    manager.set_teammate_runtime_factory([](const orangutan::orchestration::AgentSpawnRequest &) {
        struct ImmediateTeammate final : orangutan::orchestration::TeammateRuntime {
            std::string run(const std::string &, std::stop_token) override {
                return "done";
            }
        };

        return std::make_unique<ImmediateTeammate>();
    });

    const auto run = manager.spawn({
        .name = "teammate",
        .task = "trigger notification callback",
        .parent_runtime_key = "parent-runtime",
    });
    REQUIRE(run.accepted);

    REQUIRE(wait_for_condition(
        [&] {
            return callback_started.load();
        },
        100, std::chrono::milliseconds(5)));

    manager.shutdown();
}
