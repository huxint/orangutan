#include "features/tools/builtin/heartbeat.hpp"

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

TEST(HeartbeatToolTest, UpdatePreservesDeliveryScheduleStateAndActiveHoursWhenFieldsAreOmitted) {
    const auto db_path = make_test_db_path("heartbeat-tool-update.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);

    orangutan::automation::HeartbeatSpec heartbeat;
    heartbeat.id = "heartbeat-1";
    heartbeat.agent_key = "default";
    heartbeat.name = "pulse";
    heartbeat.prompt = "before";
    heartbeat.every_seconds = 3600;
    heartbeat.jitter_seconds = 300;
    heartbeat.delivery.mode = orangutan::automation::DeliveryMode::notify;
    heartbeat.delivery.targets = {"qqbot:primary:c2c:123456"};
    heartbeat.active_hours = {{.start_minute = 9 * 60, .end_minute = 17 * 60}};
    heartbeat.next_due_at = 1'763'000'000;
    const auto heartbeat_id = runtime.save_heartbeat(heartbeat);
    static_cast<void>(heartbeat_id);

    orangutan::ToolRuntimeContext context{
        .agent_key = "default",
        .automation_runtime = &runtime,
    };
    orangutan::ToolRegistry registry;
    orangutan::register_heartbeat_tool(registry, &context);

    const auto result = registry.execute(orangutan::ToolUseBlock{
        .id = "heartbeat-update",
        .name = "heartbeat",
        .input =
            {
                {"op", "update"},
                {"id", "heartbeat-1"},
                {"prompt", "after"},
            },
    });

    ASSERT_FALSE(result.is_error) << result.content;
    const auto updated = runtime.find_heartbeat("default", "heartbeat-1");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->prompt, "after");
    EXPECT_EQ(updated->delivery.mode, orangutan::automation::DeliveryMode::notify);
    ASSERT_EQ(updated->delivery.targets.size(), 1U);
    EXPECT_EQ(updated->delivery.targets.front(), "qqbot:primary:c2c:123456");
    ASSERT_EQ(updated->active_hours.size(), 1U);
    EXPECT_EQ(updated->active_hours.front().start_minute, 9 * 60);
    EXPECT_EQ(updated->active_hours.front().end_minute, 17 * 60);
    ASSERT_TRUE(updated->next_due_at.has_value());
    EXPECT_EQ(*updated->next_due_at, 1'763'000'000);
}
