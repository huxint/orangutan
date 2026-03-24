#include "features/tools/builtin/task.hpp"

#include "core/tools/tool.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include "support/ut.hpp"
#include <filesystem>
#include <string_view>

namespace {

std::filesystem::path make_test_db_path(std::string_view name) {
    return orangutan::testing::unique_test_db_path("task-tool", name);
}

boost::ut::suite task_tool_suite = [] {
    using namespace boost::ut;

    "update_preserves_delivery_when_fields_are_omitted"_test = [] {
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

        expect(not result.is_error) << result.content;
        const auto updated = runtime.find_task("default", "task-1");
        expect(updated.has_value() >> fatal) << "expected task update to persist";
        expect(updated->prompt == "after");
        expect(updated->delivery.mode == orangutan::automation::DeliveryMode::notify);
        expect(updated->delivery.targets.size() == 1_ul);
        expect(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");

        std::filesystem::remove_all(db_path.parent_path());
    };
};

} // namespace
