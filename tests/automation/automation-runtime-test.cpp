#include "automation/planner.hpp"
#include "automation/scheduler.hpp"
#include "utils/local-time.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>

using namespace std::chrono_literals;

namespace {

    std::filesystem::path make_test_db_path(std::string_view name) {
        const auto path = orangutan::testing::test_tmp_root() / std::string(name);
        std::filesystem::remove(path);
        return path;
    }

    orangutan::automation::TimePoint make_local_time(int year, int month, int day, int hour, int minute, int second) {
        const auto local_time = std::chrono::local_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} / std::chrono::day{static_cast<unsigned>(day)}} +
                                std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
        return orangutan::time::sys_time_from_local(local_time);
    }

    auto make_successful_executor(std::promise<void> &executor_called) {
        return [&executor_called](const orangutan::automation::Trigger &) {
            executor_called.set_value();
            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "ok",
                .summary = "ok",
            };
        };
    }

    TEST_CASE("cron_tasks_do_not_backfill_during_startup_minute") {
        orangutan::automation::TaskSpec task;
        task.agent_key = "default";
        task.name = "daily";
        task.prompt = "check";
        task.schedule.kind = orangutan::automation::task_schedule_kind::cron;
        task.schedule.value = "0 9 * * *";

        const auto now = make_local_time(2026, 3, 21, 9, 0, 30);

        CHECK_FALSE(orangutan::automation::is_task_due(task, now, orangutan::automation::to_unix_seconds(now)));
    };

    TEST_CASE("heartbeats_wait_for_active_agent_lease") {
        const auto db_path = make_test_db_path("automation-runtime-heartbeat-lock.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::automation::HeartbeatSpec heartbeat;
        heartbeat.id = "heartbeat-1";
        heartbeat.agent_key = "default";
        heartbeat.name = "pulse";
        heartbeat.prompt = "check";
        heartbeat.every_seconds = 60;
        heartbeat.next_due_at = orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now()) - 1;
        static_cast<void>(store.upsert_heartbeat(heartbeat));

        std::promise<void> executor_called;
        auto executor_called_future = executor_called.get_future();
        runtime.set_executor(make_successful_executor(executor_called));

        std::optional<orangutan::automation::Runtime::AgentExecutionLease> held_lease;
        held_lease.emplace(runtime.acquire_agent_execution_lease("default"));

        std::thread worker([&runtime] {
            runtime.run_pending(orangutan::automation::Clock::now());
        });

        CHECK(executor_called_future.wait_for(100ms) == std::future_status::timeout);

        held_lease.reset();

        CHECK(executor_called_future.wait_for(1s) == std::future_status::ready);

        worker.join();
    };

    TEST_CASE("tasks_wait_for_active_agent_lease") {
        const auto db_path = make_test_db_path("automation-runtime-task-lock.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        const auto scheduled_at = orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now());
        orangutan::automation::TaskSpec task;
        task.id = "task-1";
        task.agent_key = "default";
        task.name = "repo-check";
        task.prompt = "check";
        task.schedule.kind = orangutan::automation::task_schedule_kind::at;
        task.schedule.value = std::to_string(scheduled_at);
        static_cast<void>(store.upsert_task(task));

        std::promise<void> executor_called;
        auto executor_called_future = executor_called.get_future();
        runtime.set_executor(make_successful_executor(executor_called));

        std::optional<orangutan::automation::Runtime::AgentExecutionLease> held_lease;
        held_lease.emplace(runtime.acquire_agent_execution_lease("default"));

        std::thread worker([&runtime, scheduled_at] {
            runtime.run_pending(orangutan::automation::from_unix_seconds(scheduled_at));
        });

        CHECK(executor_called_future.wait_for(100ms) == std::future_status::timeout);

        held_lease.reset();

        CHECK(executor_called_future.wait_for(1s) == std::future_status::ready);

        worker.join();
    };

    TEST_CASE("manual_heartbeat_runs_reuse_current_agent_lease") {
        const auto db_path = make_test_db_path("automation-runtime-heartbeat-reentrant.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::automation::HeartbeatSpec heartbeat;
        heartbeat.id = "heartbeat-1";
        heartbeat.agent_key = "default";
        heartbeat.name = "pulse";
        heartbeat.prompt = "check";
        heartbeat.every_seconds = 60;
        heartbeat.next_due_at = orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now()) - 1;
        static_cast<void>(store.upsert_heartbeat(heartbeat));

        std::promise<void> executor_called;
        auto executor_called_future = executor_called.get_future();
        runtime.set_executor(make_successful_executor(executor_called));

        auto run_future = std::async(std::launch::async, [&runtime] {
            auto lease = runtime.acquire_agent_execution_lease("default");
            return runtime.run_heartbeat_now("default", "heartbeat-1");
        });

        REQUIRE(run_future.wait_for(1s) == std::future_status::ready);
        CHECK(run_future.get() == "Heartbeat run queued.");
        CHECK(executor_called_future.wait_for(0s) == std::future_status::ready);
    };

    TEST_CASE("manual_task_runs_reuse_current_agent_lease") {
        const auto db_path = make_test_db_path("automation-runtime-task-reentrant.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::automation::TaskSpec task;
        task.id = "task-1";
        task.agent_key = "default";
        task.name = "repo-check";
        task.prompt = "check";
        task.schedule.kind = orangutan::automation::task_schedule_kind::at;
        task.schedule.value = std::to_string(orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now()));
        static_cast<void>(store.upsert_task(task));

        std::promise<void> executor_called;
        auto executor_called_future = executor_called.get_future();
        runtime.set_executor(make_successful_executor(executor_called));

        auto run_future = std::async(std::launch::async, [&runtime] {
            auto lease = runtime.acquire_agent_execution_lease("default");
            return runtime.run_task_now("default", "task-1");
        });

        REQUIRE(run_future.wait_for(1s) == std::future_status::ready);
        CHECK(run_future.get() == "Task run queued.");
        CHECK(executor_called_future.wait_for(0s) == std::future_status::ready);
    };

} // namespace
