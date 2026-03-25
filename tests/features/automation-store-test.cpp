#include "features/automation/store.hpp"
#include "test-helpers.hpp"
#include "support/ut.hpp"

boost::ut::suite automation_json_and_store_suite = [] {
    using namespace boost::ut;
    using namespace orangutan::automation;

    "delivery_policy_json_roundtrips"_test = [] {
        const DeliveryPolicy delivery{
            .mode = DeliveryMode::notify,
            .targets = {"ops", "pager"},
        };

        const auto value = delivery_policy_to_json(delivery);
        expect(value["mode"] == "notify");
        expect(value["targets"].size() == 2_ul);
        expect(value["targets"][0] == "ops");
        expect(value["targets"][1] == "pager");

        const auto parsed = delivery_policy_from_json(value);
        expect(parsed.mode == DeliveryMode::notify);
        expect(parsed.targets.size() == 2_ul);
        expect(parsed.targets[0] == "ops");
        expect(parsed.targets[1] == "pager");
    };

    "store_roundtrips_enum_backed_fields"_test = [] {
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
        expect(loaded_task.has_value() >> fatal);
        expect(loaded_task->schedule.kind == TaskScheduleKind::at);
        expect(loaded_task->delivery.mode == DeliveryMode::notify);
        expect(loaded_task->delivery.targets.size() == 1_ul);
        expect(loaded_task->delivery.targets[0] == "ops");

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
        expect(runs.size() == 1_ul);
        expect(runs.front().kind == Kind::heartbeat);
    };
};

boost::ut::suite automation_store_suite = [] {
    using namespace boost::ut;

    "constructs_with_explicit_path"_test = [] {
        orangutan::automation::Store store(orangutan::testing::unique_test_db_path("automation-store", "store.db").string());
        static_cast<void>(store);
        expect(true);
    };
};
