#include "features/tools/builtin/heartbeat.hpp"

#include "core/tools/tool.hpp"
#include "features/automation/runtime.hpp"
#include "test-helpers.hpp"

#include "support/ut.hpp"
#include <filesystem>
#include <string_view>

namespace {

std::filesystem::path make_test_db_path(std::string_view name) {
    const auto path = orangutan::testing::test_tmp_root() / std::string(name);
    std::filesystem::remove(path);
    return path;
}

boost::ut::suite heartbeat_tool_suite = [] {
    using namespace boost::ut;

    "update_preserves_delivery_schedule_state_and_active_hours_when_fields_are_omitted"_test = [] {
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

        expect(not result.is_error) << result.content;
        const auto updated = runtime.find_heartbeat("default", "heartbeat-1");
        expect(updated.has_value() >> fatal) << "expected heartbeat update to persist";
        expect(updated->prompt == "after");
        expect(updated->delivery.mode == orangutan::automation::DeliveryMode::notify);
        expect(updated->delivery.targets.size() == 1_ul);
        expect(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");
        expect(updated->active_hours.size() == 1_ul);
        expect(updated->active_hours.front().start_minute == 9 * 60);
        expect(updated->active_hours.front().end_minute == 17 * 60);
        expect(updated->next_due_at.has_value() >> fatal) << "expected heartbeat update to preserve next due time";
        expect(*updated->next_due_at == 1'763'000'000);
    };
};

} // namespace
