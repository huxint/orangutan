#include "features/tools/builtin/task.hpp"

#include "core/tools/tool.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string_view>

namespace {

    TEST_CASE("update_preserves_delivery_when_fields_are_omitted") {
        const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-update.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::automation::TaskSpec task;
        task.id = "task-1";
        task.agent_key = "default";
        task.name = "daily";
        task.prompt = "before";
        task.schedule.kind = orangutan::automation::TaskScheduleKind::cron;
        task.schedule.value = "0 9 * * *";
        task.delivery.mode = orangutan::automation::DeliveryMode::notify;
        task.delivery.targets = {"qqbot:primary:c2c:123456"};
        static_cast<void>(runtime.save_task(task));

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::register_task_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUseBlock{
            .id = "task-update",
            .name = "task",
            .input =
                {
                    {"op", "update"},
                    {"id", "task-1"},
                    {"prompt", "after"},
                },
        });

        INFO(result.content);
        CHECK_FALSE(result.is_error);
        const auto updated = runtime.find_task("default", "task-1");
        INFO("expected task update to persist");
        REQUIRE(updated.has_value());
        CHECK(updated->prompt == "after");
        CHECK(updated->delivery.mode == orangutan::automation::DeliveryMode::notify);
        CHECK(updated->delivery.targets.size() == 1ul);
        CHECK(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");

        std::filesystem::remove_all(db_path.parent_path());
    };

} // namespace
