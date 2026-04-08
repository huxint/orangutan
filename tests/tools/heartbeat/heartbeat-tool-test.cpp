#include "tools/heartbeat/heartbeat-tool.hpp"

#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

    struct HeartbeatToolHarness {
        explicit HeartbeatToolHarness(std::string_view filename)
        : db_path(orangutan::testing::unique_test_db_path("heartbeat-tool", filename)),
          store(db_path.string()),
          runtime(store),
          context{.agent_key = "default", .automation_runtime = &runtime} {
            orangutan::tools::register_heartbeat_tool(registry, &context);
        }

        std::filesystem::path db_path;
        orangutan::automation::Store store;
        orangutan::automation::Runtime runtime;
        orangutan::ToolRuntimeContext context;
        orangutan::ToolRegistry registry;
    };

    TEST_CASE("heartbeat_unknown_op_returns_exact_error") {
        HeartbeatToolHarness harness("heartbeat-tool-unknown-op.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("heartbeat-unknown-op", "heartbeat", {{"op", "noop"}}));

        CHECK(result.content == "Error: unknown operation. Supported: add, update, remove, list, run, pause, resume.");
    }

    TEST_CASE("heartbeat_update_requires_id_or_name_with_exact_error") {
        HeartbeatToolHarness harness("heartbeat-tool-missing-id.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("heartbeat-update-missing-id", "heartbeat", {{"op", "update"}, {"prompt", "after"}}));

        CHECK(result.content == "Error: id or name is required.");
    }

    TEST_CASE("heartbeat_registered_tool_reports_unavailable_context_after_runtime_is_removed") {
        HeartbeatToolHarness harness("heartbeat-tool-runtime-removed.db");
        REQUIRE(harness.registry.find_definition("heartbeat") != nullptr);

        harness.context.automation_runtime = nullptr;

        const auto result = harness.registry.execute(orangutan::ToolUse("heartbeat-list-no-runtime", "heartbeat", {{"op", "list"}}));

        CHECK(result.content == "Error: heartbeat tool is not available in this context.");
    }

    TEST_CASE("heartbeat_is_not_registered_without_automation_runtime") {
        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = nullptr,
        };
        orangutan::ToolRegistry registry;

        orangutan::tools::register_heartbeat_tool(registry, &context);

        CHECK(registry.find_definition("heartbeat") == nullptr);
    }

    TEST_CASE("heartbeat_tool_definition_matches_exact_schema") {
        HeartbeatToolHarness harness("heartbeat-tool-definition.db");

        const auto *definition = harness.registry.find_definition("heartbeat");
        REQUIRE(definition != nullptr);
        CHECK(definition->name == "heartbeat");
        CHECK(definition->description == "Manage approximate periodic heartbeats for the current agent.");
        CHECK(definition->input_schema == nlohmann::json{
                                              {"type", "object"},
                                              {"properties",
                                               {
                                                   {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"add", "update", "remove", "list", "run", "pause", "resume"})}}},
                                                   {"id", {{"type", "string"}}},
                                                   {"name", {{"type", "string"}}},
                                                   {"every", {{"type", "string"}}},
                                                   {"jitter", {{"type", "string"}}},
                                                   {"prompt", {{"type", "string"}}},
                                                   {"notes", {{"type", "string"}}},
                                                   {"enabled", {{"type", "boolean"}}},
                                                   {"paused", {{"type", "boolean"}}},
                                                   {"delivery_mode", {{"type", "string"}, {"enum", nlohmann::json::array({"silent", "notify"})}}},
                                                   {"targets", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                                   {"active_hours", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                                               }},
                                              {"required", nlohmann::json::array({"op"})},
                                          });
    }

    TEST_CASE("update_preserves_delivery_schedule_state_and_active_hours_when_fields_are_omitted") {
        HeartbeatToolHarness harness("heartbeat-tool-update.db");

        orangutan::automation::HeartbeatSpec heartbeat;
        heartbeat.id = "heartbeat-1";
        heartbeat.agent_key = "default";
        heartbeat.name = "pulse";
        heartbeat.prompt = "before";
        heartbeat.every_seconds = 3600;
        heartbeat.jitter_seconds = 300;
        heartbeat.delivery.mode = orangutan::automation::delivery_mode::notify;
        heartbeat.delivery.targets = {"qqbot:primary:c2c:123456"};
        heartbeat.active_hours = {{.start_minute = 9 * 60, .end_minute = 17 * 60}};
        heartbeat.next_due_at = 1'763'000'000;
        const auto heartbeat_id = harness.runtime.save_heartbeat(heartbeat);
        static_cast<void>(heartbeat_id);

        const auto result = harness.registry.execute(orangutan::ToolUse("heartbeat-update", "heartbeat",
                                                                        {
                                                                            {"op", "update"},
                                                                            {"id", "heartbeat-1"},
                                                                            {"prompt", "after"},
                                                                        }));

        INFO(result.content);
        CHECK_FALSE(result.is_error);
        const auto updated = harness.runtime.find_heartbeat("default", "heartbeat-1");
        INFO("expected heartbeat update to persist");
        REQUIRE(updated.has_value());
        CHECK(updated->prompt == "after");
        CHECK(updated->delivery.mode == orangutan::automation::delivery_mode::notify);
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
