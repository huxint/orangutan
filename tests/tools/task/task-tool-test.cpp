#include "tools/task/task-tool.hpp"

#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

    struct TaskToolHarness {
        explicit TaskToolHarness(std::string_view filename)
        : db_path(orangutan::testing::unique_test_db_path("task-tool", filename)),
          store(db_path.string()),
          runtime(store),
          context{.agent_key = "default", .automation_runtime = &runtime} {
            orangutan::tools::register_task_tool(registry, &context);
        }

        std::filesystem::path db_path;
        orangutan::automation::Store store;
        orangutan::automation::Runtime runtime;
        orangutan::ToolRuntimeContext context;
        orangutan::ToolRegistry registry;
    };

    TEST_CASE("task_unknown_op_returns_exact_error") {
        TaskToolHarness harness("task-tool-unknown-op.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("task-unknown-op", "task", {{"op", "noop"}}));

        CHECK(result.content == "Error: unknown operation. Supported: add, update, remove, list, run.");
    }

    TEST_CASE("task_update_requires_id_or_name_with_exact_error") {
        TaskToolHarness harness("task-tool-missing-id.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("task-update-missing-id", "task", {{"op", "update"}, {"prompt", "after"}}));

        CHECK(result.content == "Error: id or name is required.");
    }

    TEST_CASE("task_registered_tool_reports_unavailable_context_at_execute_time") {
        TaskToolHarness harness("task-tool-unavailable-context.db");

        harness.context.automation_runtime = nullptr;
        const auto result = harness.registry.execute(orangutan::ToolUse("task-list-unavailable", "task", {{"op", "list"}}));

        CHECK(result.content == "Error: task tool is not available in this context.");
    }

    TEST_CASE("task_is_not_registered_without_automation_runtime") {
        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = nullptr,
        };
        orangutan::ToolRegistry registry;

        orangutan::tools::register_task_tool(registry, &context);

        CHECK(registry.find_definition("task") == nullptr);
    }

    TEST_CASE("task_tool_definition_matches_exact_schema") {
        TaskToolHarness harness("task-tool-definition.db");

        const auto *definition = harness.registry.find_definition("task");
        REQUIRE(definition != nullptr);
        CHECK(definition->name == "task");
        CHECK(definition->description == "Manage precise scheduled tasks for the current agent.");
        CHECK(definition->input_schema == nlohmann::json{
                                              {"type", "object"},
                                              {"properties",
                                               {
                                                   {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"add", "update", "remove", "list", "run"})}}},
                                                   {"id", {{"type", "string"}}},
                                                   {"name", {{"type", "string"}}},
                                                   {"schedule_kind", {{"type", "string"}, {"enum", nlohmann::json::array({"at", "cron"})}}},
                                                   {"schedule", {{"type", "string"}}},
                                                   {"prompt", {{"type", "string"}}},
                                                   {"notes", {{"type", "string"}}},
                                                   {"enabled", {{"type", "boolean"}}},
                                                   {"delivery_mode", {{"type", "string"}, {"enum", nlohmann::json::array({"silent", "notify"})}}},
                                                   {"targets", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                               }},
                                              {"required", nlohmann::json::array({"op"})},
                                          });
    }

    TEST_CASE("update_preserves_delivery_when_fields_are_omitted") {
        TaskToolHarness harness("task-tool-update.db");

        orangutan::automation::TaskSpec task;
        task.id = "task-1";
        task.agent_key = "default";
        task.name = "daily";
        task.prompt = "before";
        task.schedule.kind = orangutan::automation::task_schedule_kind::cron;
        task.schedule.value = "0 9 * * *";
        task.delivery.mode = orangutan::automation::delivery_mode::notify;
        task.delivery.targets = {"qqbot:primary:c2c:123456"};
        static_cast<void>(harness.runtime.save_task(task));

        const auto result = harness.registry.execute(orangutan::ToolUse("task-update", "task",
                                                                        {
                                                                            {"op", "update"},
                                                                            {"id", "task-1"},
                                                                            {"prompt", "after"},
                                                                        }));

        INFO(result.content);
        CHECK_FALSE(result.is_error);
        const auto updated = harness.runtime.find_task("default", "task-1");
        INFO("expected task update to persist");
        REQUIRE(updated.has_value());
        CHECK(updated->prompt == "after");
        CHECK(updated->delivery.mode == orangutan::automation::delivery_mode::notify);
        CHECK(updated->delivery.targets.size() == 1UL);
        CHECK(updated->delivery.targets.front() == "qqbot:primary:c2c:123456");
    };

} // namespace
