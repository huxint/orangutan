#include "features/automation/store.hpp"
#include "test-helpers.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace orangutan::automation;

TEST_CASE("delivery_policy_json_roundtrips") {
    const DeliveryPolicy delivery{
        .mode = DeliveryMode::notify,
        .targets = {"ops", "pager"},
    };

    const auto value = delivery_policy_to_json(delivery);
    CHECK(value["mode"] == "notify");
    CHECK(value["targets"].size() == 2ul);
    CHECK(value["targets"][0] == "ops");
    CHECK(value["targets"][1] == "pager");

    const auto parsed = delivery_policy_from_json(value);
    CHECK(parsed.mode == DeliveryMode::notify);
    CHECK(parsed.targets.size() == 2ul);
    CHECK(parsed.targets[0] == "ops");
    CHECK(parsed.targets[1] == "pager");
};

TEST_CASE("store_roundtrips_enum_backed_fields") {
    const auto db_path = orangutan::testing::unique_test_db_path("automation-store", "roundtrip.db");
    Store store(db_path);

    TaskSpec task{
        .agent_key = "default",
        .name = "once",
        .schedule = {.kind = TaskScheduleKind::at, .value = "1735689600"},
        .prompt = "run once",
        .delivery = {.mode = DeliveryMode::notify, .targets = {"ops"}},
    };

    const auto task_id = store.upsert_task(task);
    const auto loaded_task = store.find_task("default", task_id);
    REQUIRE(loaded_task.has_value());
    CHECK(loaded_task->schedule.kind == TaskScheduleKind::at);
    CHECK(loaded_task->delivery.mode == DeliveryMode::notify);
    CHECK(loaded_task->delivery.targets.size() == 1UL);
    CHECK(loaded_task->delivery.targets[0] == "ops");

    static_cast<void>(store.insert_run(RunRecord{
        .id = "run-1",
        .kind = Kind::heartbeat,
        .automation_id = task_id,
        .agent_key = "default",
        .automation_name = "once",
        .started_at = 1735689600,
        .status = "succeeded",
        .summary = "ok",
    }));

    const auto runs = store.list_runs("default");
    CHECK(runs.size() == 1UL);
    CHECK(runs.front().kind == Kind::heartbeat);
};

TEST_CASE("constructs_with_explicit_path") {
    orangutan::automation::Store store(orangutan::testing::unique_test_db_path("automation-store", "store.db").string());
    static_cast<void>(store);
    CHECK(true);
};
