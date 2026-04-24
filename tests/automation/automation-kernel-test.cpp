#include <chrono>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "automation/kernel.hpp"
#include "automation/sqlite-store.hpp"
#include "test-helpers.hpp"

namespace {

    using orangutan::automation::ActionDescriptor;
    using orangutan::automation::DispatchReason;
    using orangutan::automation::ExecutionPolicy;
    using orangutan::automation::ExecutionResult;
    using orangutan::automation::IntervalSchedule;
    using orangutan::automation::JobDefinition;
    using orangutan::automation::JobId;
    using orangutan::automation::Kernel;
    using orangutan::automation::MissedRunPolicy;
    using orangutan::automation::ScheduleState;
    using orangutan::automation::SqliteJobStore;

    [[nodiscard]]
    JobDefinition make_interval_definition(std::string_view id, std::string_view key, std::chrono::seconds every, MissedRunPolicy missed_runs = MissedRunPolicy::run_once) {
        return JobDefinition{
            .id = JobId{.value = std::string(id)},
            .key = std::string(key),
            .schedule =
                IntervalSchedule{
                    .every = every,
                    .jitter = std::chrono::seconds{0},
                    .active_windows = {},
                    .time_zone = "UTC",
                },
            .action =
                ActionDescriptor{
                    .action_key = "agent.prompt",
                    .payload = {
                        {"agent", "default"},
                        {"prompt", "scan repo"},
                    },
                },
            .execution =
                ExecutionPolicy{
                    .missed_runs = missed_runs,
                },
        };
    }

    [[nodiscard]]
    ScheduleState make_state(std::int64_t next_due_at) {
        return ScheduleState{
            .next_due_at = next_due_at,
        };
    }

    TEST_CASE("kernel reports earliest wakeup from store state", "[automation][kernel]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-kernel", "next-wakeup.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(1'000)).has_value());
        REQUIRE(store.save_job(make_interval_definition("job-2", "daily-report", std::chrono::seconds{30}), make_state(1'200)).has_value());

        auto wakeup = kernel.next_wakeup();
        REQUIRE(wakeup.has_value());
        REQUIRE(wakeup->has_value());
        CHECK(orangutan::automation::to_unix_seconds(**wakeup) == 1'000);
    }

    TEST_CASE("kernel emits one recovery dispatch for run_once missed policy", "[automation][kernel]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-kernel", "run-once.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}, MissedRunPolicy::run_once), make_state(1'000)).has_value());

        auto dispatches = kernel.reserve_due(orangutan::automation::from_unix_seconds(1'105), 1, "driver-a");
        REQUIRE(dispatches.has_value());
        REQUIRE(dispatches->size() == 1UL);
        CHECK(dispatches->front().job_id.value == "job-1");
        CHECK(orangutan::automation::to_unix_seconds(dispatches->front().scheduled_for) == 1'000);
        CHECK(dispatches->front().reason == DispatchReason::catch_up);
    }

    TEST_CASE("kernel mark_started updates in-flight scheduler state", "[automation][kernel]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-kernel", "mark-started.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(1'000)).has_value());

        auto dispatches = kernel.reserve_due(orangutan::automation::from_unix_seconds(1'000), 1, "driver-a");
        REQUIRE(dispatches.has_value());
        REQUIRE(dispatches->size() == 1UL);

        auto started = kernel.mark_started(dispatches->front().execution_id, orangutan::automation::from_unix_seconds(1'005));
        REQUIRE(started.has_value());

        auto loaded = store.load_job(JobId{.value = "job-1"});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        REQUIRE(loaded->value().state.last_started_at.has_value());
        CHECK(*loaded->value().state.last_started_at == 1'005);
        CHECK(loaded->value().state.in_flight_count == 1);
    }

    TEST_CASE("kernel mark_finished advances next due and clears lease", "[automation][kernel]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-kernel", "mark-finished.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(1'000)).has_value());

        auto dispatches = kernel.reserve_due(orangutan::automation::from_unix_seconds(1'000), 1, "driver-a");
        REQUIRE(dispatches.has_value());
        REQUIRE(dispatches->size() == 1UL);

        REQUIRE(kernel.mark_started(dispatches->front().execution_id, orangutan::automation::from_unix_seconds(1'005)).has_value());
        REQUIRE(kernel
                    .mark_finished(
                        dispatches->front().execution_id,
                        ExecutionResult{
                            .success = true,
                            .summary = "ok",
                        },
                        orangutan::automation::from_unix_seconds(1'015))
                    .has_value());

        auto loaded = store.load_job(JobId{.value = "job-1"});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        REQUIRE(loaded->value().state.last_finished_at.has_value());
        CHECK(*loaded->value().state.last_finished_at == 1'015);
        CHECK(loaded->value().state.last_status == "completed");
        CHECK(loaded->value().state.in_flight_count == 0);
        CHECK(loaded->value().state.lease_owner.empty());
        CHECK_FALSE(loaded->value().state.lease_expires_at.has_value());
        REQUIRE(loaded->value().state.next_due_at.has_value());
        CHECK(*loaded->value().state.next_due_at == 1'045);
    }

    TEST_CASE("kernel recover drops expired local reservations", "[automation][kernel]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-kernel", "recover.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store, std::chrono::seconds{30});

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(1'000)).has_value());

        auto dispatches = kernel.reserve_due(orangutan::automation::from_unix_seconds(1'000), 1, "driver-a");
        REQUIRE(dispatches.has_value());
        REQUIRE(dispatches->size() == 1UL);
        const auto stale_execution_id = dispatches->front().execution_id;

        REQUIRE(kernel.recover(orangutan::automation::from_unix_seconds(1'031), "driver-a").has_value());

        auto started = kernel.mark_started(stale_execution_id, orangutan::automation::from_unix_seconds(1'032));
        CHECK_FALSE(started.has_value());

        auto refreshed = kernel.reserve_due(orangutan::automation::from_unix_seconds(1'031), 1, "driver-b");
        REQUIRE(refreshed.has_value());
        REQUIRE(refreshed->size() == 1UL);
        CHECK(refreshed->front().execution_id.value != stale_execution_id.value);
        CHECK(refreshed->front().job_id.value == "job-1");
    }

} // namespace
