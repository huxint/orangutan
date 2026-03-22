#include "features/automation/planner.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
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
    std::tm local_tm{};
    local_tm.tm_year = year - 1900;
    local_tm.tm_mon = month - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = second;
    return orangutan::automation::from_unix_seconds(static_cast<std::int64_t>(std::mktime(&local_tm)));
}

} // namespace

TEST(AutomationRuntimeTest, CronTasksDoNotBackfillDuringStartupMinute) {
    orangutan::automation::TaskSpec task;
    task.agent_key = "default";
    task.name = "daily";
    task.prompt = "check";
    task.schedule.kind = orangutan::automation::TaskScheduleKind::cron;
    task.schedule.value = "0 9 * * *";

    const auto now = make_local_time(2026, 3, 21, 9, 0, 30);

    EXPECT_FALSE(orangutan::automation::is_task_due(task, now, orangutan::automation::to_unix_seconds(now)));
}

TEST(AutomationRuntimeTest, HeartbeatsWaitForActiveAgentLease) {
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
    const auto heartbeat_id = store.upsert_heartbeat(heartbeat);
    static_cast<void>(heartbeat_id);

    std::promise<void> executor_called;
    auto executor_called_future = executor_called.get_future();
    runtime.set_executor([&executor_called](const orangutan::automation::Trigger &) {
        executor_called.set_value();
        return orangutan::automation::ExecutionResult{
            .success = true,
            .reply = "ok",
            .summary = "ok",
        };
    });

    std::optional<orangutan::automation::Runtime::AgentExecutionLease> held_lease;
    held_lease.emplace(runtime.acquire_agent_execution_lease("default"));

    std::thread worker([&runtime] {
        runtime.run_pending(orangutan::automation::Clock::now());
    });

    EXPECT_EQ(executor_called_future.wait_for(100ms), std::future_status::timeout);

    held_lease.reset();

    EXPECT_EQ(executor_called_future.wait_for(1s), std::future_status::ready);

    worker.join();
}

TEST(AutomationRuntimeTest, TasksWaitForActiveAgentLease) {
    const auto db_path = make_test_db_path("automation-runtime-task-lock.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);

    const auto scheduled_at = orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now());
    orangutan::automation::TaskSpec task;
    task.id = "task-1";
    task.agent_key = "default";
    task.name = "repo-check";
    task.prompt = "check";
    task.schedule.kind = orangutan::automation::TaskScheduleKind::at;
    task.schedule.value = std::to_string(scheduled_at);
    const auto task_id = store.upsert_task(task);
    static_cast<void>(task_id);

    std::promise<void> executor_called;
    auto executor_called_future = executor_called.get_future();
    runtime.set_executor([&executor_called](const orangutan::automation::Trigger &) {
        executor_called.set_value();
        return orangutan::automation::ExecutionResult{
            .success = true,
            .reply = "ok",
            .summary = "ok",
        };
    });

    std::optional<orangutan::automation::Runtime::AgentExecutionLease> held_lease;
    held_lease.emplace(runtime.acquire_agent_execution_lease("default"));

    std::thread worker([&runtime, scheduled_at] {
        runtime.run_pending(orangutan::automation::from_unix_seconds(scheduled_at));
    });

    EXPECT_EQ(executor_called_future.wait_for(100ms), std::future_status::timeout);

    held_lease.reset();

    EXPECT_EQ(executor_called_future.wait_for(1s), std::future_status::ready);

    worker.join();
}

TEST(AutomationRuntimeTest, ManualHeartbeatRunsReuseCurrentAgentLease) {
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
    const auto heartbeat_id = store.upsert_heartbeat(heartbeat);
    static_cast<void>(heartbeat_id);

    std::promise<void> executor_called;
    auto executor_called_future = executor_called.get_future();
    runtime.set_executor([&executor_called](const orangutan::automation::Trigger &) {
        executor_called.set_value();
        return orangutan::automation::ExecutionResult{
            .success = true,
            .reply = "ok",
            .summary = "ok",
        };
    });

    auto run_future = std::async(std::launch::async, [&runtime] {
        auto lease = runtime.acquire_agent_execution_lease("default");
        return runtime.run_heartbeat_now("default", "heartbeat-1");
    });

    ASSERT_EQ(run_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(run_future.get(), "Heartbeat run queued.");
    EXPECT_EQ(executor_called_future.wait_for(0s), std::future_status::ready);
}

TEST(AutomationRuntimeTest, ManualTaskRunsReuseCurrentAgentLease) {
    const auto db_path = make_test_db_path("automation-runtime-task-reentrant.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);

    orangutan::automation::TaskSpec task;
    task.id = "task-1";
    task.agent_key = "default";
    task.name = "repo-check";
    task.prompt = "check";
    task.schedule.kind = orangutan::automation::TaskScheduleKind::at;
    task.schedule.value = std::to_string(orangutan::automation::to_unix_seconds(orangutan::automation::Clock::now()));
    const auto task_id = store.upsert_task(task);
    static_cast<void>(task_id);

    std::promise<void> executor_called;
    auto executor_called_future = executor_called.get_future();
    runtime.set_executor([&executor_called](const orangutan::automation::Trigger &) {
        executor_called.set_value();
        return orangutan::automation::ExecutionResult{
            .success = true,
            .reply = "ok",
            .summary = "ok",
        };
    });

    auto run_future = std::async(std::launch::async, [&runtime] {
        auto lease = runtime.acquire_agent_execution_lease("default");
        return runtime.run_task_now("default", "task-1");
    });

    ASSERT_EQ(run_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(run_future.get(), "Task run queued.");
    EXPECT_EQ(executor_called_future.wait_for(0s), std::future_status::ready);
}
