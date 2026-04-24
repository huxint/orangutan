#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "automation/core-model.hpp"
#include "automation/sqlite-store.hpp"
#include "automation/store.hpp"
#include "test-helpers.hpp"

namespace {

    using orangutan::automation::ActionDescriptor;
    using orangutan::automation::CronSchedule;
    using orangutan::automation::JobDefinition;
    using orangutan::automation::JobId;
    using orangutan::automation::ScheduleState;
    using orangutan::automation::SqliteJobStore;

    [[nodiscard]]
    JobDefinition make_definition(std::string_view id, std::string_view key) {
        return JobDefinition{
            .id = JobId{.value = std::string(id)},
            .key = std::string(key),
            .schedule =
                CronSchedule{
                    .expr = "0 * * * *",
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
        };
    }

    [[nodiscard]]
    ScheduleState make_state(std::int64_t next_due_at) {
        return ScheduleState{
            .next_due_at = next_due_at,
        };
    }

    TEST_CASE("sqlite store round-trips core jobs", "[automation][store]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-store", "roundtrip.db");
        SqliteJobStore store(db_path);

        const auto definition = make_definition("job-1", "repo-sync");
        const auto state = make_state(1'776'249'600);

        REQUIRE(store.save_job(definition, state).has_value());

        auto loaded = store.load_job(definition.id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        CHECK(loaded->value().definition.id.value == "job-1");
        CHECK(loaded->value().definition.key == "repo-sync");
        REQUIRE(loaded->value().state.next_due_at.has_value());
        CHECK(*loaded->value().state.next_due_at == 1'776'249'600);
    }

    TEST_CASE("sqlite store returns earliest next_due_at", "[automation][store]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-store", "next-due.db");
        SqliteJobStore store(db_path);

        REQUIRE(store.save_job(make_definition("job-1", "repo-sync"), make_state(1'776'249'900)).has_value());
        REQUIRE(store.save_job(make_definition("job-2", "daily-report"), make_state(1'776'249'600)).has_value());

        auto next_due_at = store.next_due_at();
        REQUIRE(next_due_at.has_value());
        REQUIRE(next_due_at->has_value());
        CHECK(**next_due_at == 1'776'249'600);
    }

    TEST_CASE("sqlite store lists due jobs in time order", "[automation][store]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-store", "due-jobs.db");
        SqliteJobStore store(db_path);

        REQUIRE(store.save_job(make_definition("job-1", "repo-sync"), make_state(1'776'249'900)).has_value());
        REQUIRE(store.save_job(make_definition("job-2", "daily-report"), make_state(1'776'249'600)).has_value());
        REQUIRE(store.save_job(make_definition("job-3", "late-job"), make_state(1'776'250'100)).has_value());

        auto due = store.list_due(1'776'249'950, 2);
        REQUIRE(due.has_value());
        REQUIRE(due->size() == 2UL);
        CHECK(due->at(0).value == "job-2");
        CHECK(due->at(1).value == "job-1");
    }

    TEST_CASE("sqlite store removes stored jobs", "[automation][store]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-store", "remove.db");
        SqliteJobStore store(db_path);

        const auto definition = make_definition("job-1", "repo-sync");
        REQUIRE(store.save_job(definition, make_state(1'776'249'600)).has_value());

        auto removed = store.remove_job(definition.id);
        REQUIRE(removed.has_value());
        CHECK(*removed);

        auto loaded = store.load_job(definition.id);
        REQUIRE(loaded.has_value());
        CHECK_FALSE(loaded->has_value());
    }

} // namespace
