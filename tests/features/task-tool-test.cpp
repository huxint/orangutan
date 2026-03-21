#include "features/tools/builtin/task.hpp"

#include "core/tools/tool.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

namespace {

std::filesystem::path make_test_db_path(std::string_view name) {
    const auto path = orangutan::testing::test_tmp_root() / std::string(name);
    std::filesystem::remove(path);
    return path;
}

} // namespace

TEST(TaskToolTest, UpdatePreservesDeliveryWhenFieldsAreOmitted) {
    const auto db_path = make_test_db_path("task-tool-update.db");
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
    const auto task_id = runtime.save_task(task);
    (void)task_id;

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

    ASSERT_FALSE(result.is_error) << result.content;
    const auto updated = runtime.find_task("default", "task-1");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->prompt, "after");
    EXPECT_EQ(updated->delivery.mode, orangutan::automation::DeliveryMode::notify);
    ASSERT_EQ(updated->delivery.targets.size(), 1U);
    EXPECT_EQ(updated->delivery.targets.front(), "qqbot:primary:c2c:123456");
}
