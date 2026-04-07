#include "tools/task/task-tool.hpp"

#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string_view>

namespace {

    TEST_CASE("task_unknown_op_returns_exact_error") {
        const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-unknown-op.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_task_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUse("task-unknown-op", "task", {{"op", "noop"}}));

        CHECK(result.content == "Error: unknown operation. Supported: add, update, remove, list, run.");
    }

    TEST_CASE("task_update_requires_id_or_name_with_exact_error") {
        const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-missing-id.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_task_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUse("task-update-missing-id", "task", {{"op", "update"}, {"prompt", "after"}}));

        CHECK(result.content == "Error: id or name is required.");
    }

    TEST_CASE("task_registered_tool_reports_unavailable_context_at_execute_time") {
        const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-unavailable-context.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_task_tool(registry, &context);

        context.automation_runtime = nullptr;
        const auto result = registry.execute(orangutan::ToolUse("task-list-unavailable", "task", {{"op", "list"}}));

        CHECK(result.content == "Error: task tool is not available in this context.");
    }

    TEST_CASE("update_preserves_delivery_when_fields_are_omitted") {
        const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-update.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::automation::TaskSpec task;
        task.id = "task-1";
        task.agent_key = "default";
        task.name = "daily";
        task.prompt = "before";
        task.schedule.kind = orangutan::automation::task_schedule_kind::cron;
        task.schedule.value = "0 9 * * *";
        task.delivery.mode = orangutan::automation::delivery_mode::notify;
        task.delivery.targets = {"qqbot:primary:c2c:123456"};
        static_cast<void>(runtime.save_task(task));

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_task_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUse("task-update", "task",
                                                                {
                                                                    {"op", "update"},
                                                                    {"id", "task-1"},
                                                                    {"prompt", "after"},
                                                                }));

        INFO(result.content);
        CHECK_FALSE(result.is_error);
        const auto updated = runtime.find_task("default", "task-1");
        INFO("expected task update to persist");
        REQUIRE(updated.has_value());
        CHECK(updated->prompt == "after");
        CHECK(updated->delivery.mode == orangutan::automation::delivery_mode::notify);
        CHECK(updated->delivery.targets.size() == 1UL);
        CHECK(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");

        std::filesystem::remove_all(db_path.parent_path());
    };

} // namespace
