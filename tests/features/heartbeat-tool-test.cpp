#include "tools/heartbeat/heartbeat-tool.hpp"

#include "tools/registry/tool.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string_view>

namespace {

    std::filesystem::path make_test_db_path(std::string_view name) {
        const auto path = orangutan::testing::test_tmp_root() / std::string(name);
        std::filesystem::remove(path);
        return path;
    }

    TEST_CASE("update_preserves_delivery_schedule_state_and_active_hours_when_fields_are_omitted") {
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

        const auto result = registry.execute(orangutan::ToolUse("heartbeat-update", "heartbeat",
                                                                {
                                                                    {"op", "update"},
                                                                    {"id", "heartbeat-1"},
                                                                    {"prompt", "after"},
                                                                }));

        INFO(result.content);
        CHECK_FALSE(result.is_error);
        const auto updated = runtime.find_heartbeat("default", "heartbeat-1");
        INFO("expected heartbeat update to persist");
        REQUIRE(updated.has_value());
        CHECK(updated->prompt == "after");
        CHECK(updated->delivery.mode == orangutan::automation::DeliveryMode::notify);
        CHECK(updated->delivery.targets.size() == 1UL);
        CHECK(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");
        CHECK(updated->active_hours.size() == 1UL);
        CHECK(updated->active_hours.front().start_minute == 9 * 60);
        CHECK(updated->active_hours.front().end_minute == 17 * 60);
        INFO("expected heartbeat update to preserve next due time");
        REQUIRE(updated->next_due_at.has_value());
        CHECK(*updated->next_due_at == 1'763'000'000);
    };

} // namespace
