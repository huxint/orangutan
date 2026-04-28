#include "tools/automation/automation-tool.hpp"

#include "automation/repository.hpp"
#include "automation/service.hpp"
#include "test-helpers.hpp"
#include "tools/register.hpp"
#include "tools/registry/tool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using RegisterAutomationWithContext = void (*)(orangutan::ToolRegistry &, const orangutan::ToolRuntimeContext *);
    using RegisterAutomationWithCapability = void (*)(orangutan::ToolRegistry &, orangutan::tools::AutomationCapability);

    static_assert(std::same_as<decltype(static_cast<RegisterAutomationWithContext>(&orangutan::tools::register_automation_tool)), RegisterAutomationWithContext>);
    static_assert(std::same_as<decltype(static_cast<RegisterAutomationWithCapability>(&orangutan::tools::register_automation_tool)), RegisterAutomationWithCapability>);

    struct NotificationCall {
        std::string target;
        std::string body;
    };

    struct AutomationToolHarness {
        explicit AutomationToolHarness(std::string_view filename)
        : db_path(orangutan::testing::unique_test_db_path("automation-tool", filename)),
          repository(db_path),
          service(repository, [this] {
              return current_time;
          }),
          context{.agent_key = "default", .automation_service = &service} {
            service.set_executor([this](const orangutan::automation::Automation &automation) {
                executed_ids.push_back(automation.id);
                return orangutan::automation::ExecutionResult{
                    .success = true,
                    .reply = "ok",
                    .summary = "ok",
                };
            });
            service.set_notifier([this](std::string_view target, std::string_view, std::string_view body) -> std::optional<std::string> {
                notifications.push_back({
                    .target = std::string(target),
                    .body = std::string(body),
                });
                return std::nullopt;
            });
            orangutan::tools::register_automation_tool(registry, &context);
        }

        [[nodiscard]]
        orangutan::ToolResult invoke(const nlohmann::json &input) {
            return registry.execute(orangutan::ToolUse("automation-tool", "automation", input));
        }

        [[nodiscard]]
        nlohmann::json invoke_json(const nlohmann::json &input) {
            return nlohmann::json::parse(invoke(input).content);
        }

        std::filesystem::path db_path;
        orangutan::automation::Repository repository;
        orangutan::automation::TimePoint current_time = orangutan::automation::from_unix_seconds(1'000);
        orangutan::automation::AutomationService service;
        orangutan::ToolRuntimeContext context;
        orangutan::ToolRegistry registry;
        std::vector<std::string> executed_ids;
        std::vector<NotificationCall> notifications;
    };

    TEST_CASE("automation_unknown_op_returns_exact_error") {
        AutomationToolHarness harness("automation-tool-unknown-op.db");

        const auto result = harness.invoke({{"op", "noop"}});

        CHECK(result.content ==
              "Error: unknown operation. Supported: create, update, get, list, remove, run, pause, resume, list_runs, list_deliveries, ack_delivery, clear_deliveries.");
    };

    TEST_CASE("automation_registered_tool_reports_unavailable_context_at_execute_time") {
        AutomationToolHarness harness("automation-tool-unavailable.db");
        REQUIRE(harness.registry.find_definition("automation") != nullptr);

        harness.context.automation_service = nullptr;
        const auto result = harness.invoke({{"op", "list"}});

        CHECK(result.content == "Error: automation tool is not available in this context.");
    };

    TEST_CASE("builtin_automation_registration_reads_context_at_execute_time") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-tool", "builtin-live-context.db");
        orangutan::automation::Repository repository(db_path);
        orangutan::automation::AutomationService service(repository, [] {
            return orangutan::automation::from_unix_seconds(1'000);
        });
        orangutan::ToolRuntimeContext context{.agent_key = "default", .automation_service = &service};
        orangutan::ToolRegistry registry;

        orangutan::tools::register_builtin_tools(registry, nullptr, orangutan::testing::test_tmp_root(), &context, nullptr);
        REQUIRE(registry.find_definition("automation") != nullptr);

        context.automation_service = nullptr;
        const auto result = registry.execute(orangutan::ToolUse("automation-tool", "automation", {{"op", "list"}}));

        CHECK(result.content == "Error: automation tool is not available in this context.");

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("automation_is_not_registered_without_automation_service") {
        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_service = nullptr,
        };
        orangutan::ToolRegistry registry;

        orangutan::tools::register_automation_tool(registry, &context);

        CHECK(registry.find_definition("automation") == nullptr);
    };

    TEST_CASE("automation_is_not_registered_without_automation_service_capability") {
        orangutan::ToolRegistry registry;

        orangutan::tools::register_automation_tool(registry,
                                                   orangutan::tools::AutomationCapability{
                                                       .agent_key = "default",
                                                   });

        CHECK(registry.find_definition("automation") == nullptr);
    };

    TEST_CASE("automation_tool_creates_gets_lists_and_removes_automations") {
        AutomationToolHarness harness("automation-tool-crud.db");

        const auto create_result = harness.invoke({
            {"op", "create"},
            {"name", "repo-check"},
            {"prompt", "scan repo"},
            {"trigger", {{"type", "cron"}, {"cron", "0 9 * * *"}}},
            {"delivery", {{"mode", "notify"}, {"targets", {"owner"}}}},
        });
        CHECK_FALSE(create_result.is_error);

        const auto listed = harness.invoke_json({{"op", "list"}});
        REQUIRE(listed.is_array());
        REQUIRE(listed.size() == 1UL);
        const auto automation_id = listed.at(0).at("id").get<std::string>();
        CHECK(listed.at(0).at("name") == "repo-check");

        const auto fetched = harness.invoke_json({{"op", "get"}, {"id", automation_id}});
        CHECK(fetched.at("prompt") == "scan repo");
        CHECK(fetched.at("delivery").at("mode") == "notify");

        const auto remove_result = harness.invoke({{"op", "remove"}, {"id", automation_id}});
        CHECK_FALSE(remove_result.is_error);
        CHECK(remove_result.content == "Removed automation.");
        CHECK(harness.invoke_json({{"op", "list"}}).empty());
    };

    TEST_CASE("automation_tool_update_preserves_omitted_fields_and_replaces_nested_objects") {
        AutomationToolHarness harness("automation-tool-update.db");

        static_cast<void>(harness.invoke({
            {"op", "create"},
            {"name", "repo-check"},
            {"prompt", "scan repo"},
            {"notes", "daily"},
            {"trigger", {{"type", "cron"}, {"cron", "0 9 * * *"}, {"time_zone", "Asia/Shanghai"}}},
            {"delivery", {{"mode", "notify"}, {"targets", {"owner"}}}},
            {"tags", {"daily"}},
        }));

        const auto response = harness.invoke({
            {"op", "update"},
            {"name", "repo-check"},
            {"delivery", {{"mode", "silent"}, {"targets", nlohmann::json::array()}}},
        });
        CHECK_FALSE(response.is_error);
        CHECK(response.content.find("Updated automation") != std::string::npos);

        auto stored = harness.service.find("default", "repo-check");
        REQUIRE(stored.has_value());
        CHECK(stored->prompt == "scan repo");
        CHECK(stored->notes == "daily");
        CHECK(stored->delivery.mode == orangutan::automation::delivery_mode::silent);
        CHECK(stored->delivery.targets.empty());

        static_cast<void>(harness.invoke({
            {"op", "update"},
            {"name", "repo-check"},
            {"trigger", {{"type", "once"}, {"at", "2026-04-15T00:00:00Z"}}},
        }));

        stored = harness.service.find("default", "repo-check");
        REQUIRE(stored.has_value());
        CHECK(stored->trigger.type == orangutan::automation::trigger_type::once);
        CHECK(stored->trigger.cron.empty());
        CHECK(stored->trigger.every == std::chrono::seconds{0});
        CHECK(stored->tags.size() == 1UL);
        CHECK(stored->tags.front() == "daily");
    };

    TEST_CASE("automation_selector_prefers_exact_id_before_name") {
        AutomationToolHarness harness("automation-tool-selector.db");

        static_cast<void>(harness.invoke({
            {"op", "create"},
            {"name", "repo-check"},
            {"prompt", "scan repo"},
            {"trigger", {{"type", "cron"}, {"cron", "0 9 * * *"}}},
        }));

        auto by_name = harness.service.find("default", "repo-check");
        REQUIRE(by_name.has_value());

        static_cast<void>(harness.invoke({
            {"op", "create"},
            {"name", by_name->id},
            {"prompt", "scan conflicting id"},
            {"trigger", {{"type", "cron"}, {"cron", "0 10 * * *"}}},
        }));

        const auto fetched = harness.invoke_json({{"op", "get"}, {"id", by_name->id}});
        CHECK(fetched.at("name") == "repo-check");
    };

    TEST_CASE("automation_tool_runs_pauses_resumes_and_manages_delivery_history") {
        AutomationToolHarness harness("automation-tool-runtime.db");

        static_cast<void>(harness.invoke({
            {"op", "create"},
            {"name", "repo-check"},
            {"prompt", "scan repo"},
            {"trigger", {{"type", "interval"}, {"every", "30s"}, {"jitter", "0s"}}},
            {"delivery", {{"mode", "notify"}, {"targets", {"owner", "pager"}}}},
        }));

        auto stored = harness.service.find("default", "repo-check");
        REQUIRE(stored.has_value());

        auto pause_result = harness.invoke({{"op", "pause"}, {"id", stored->id}});
        CHECK_FALSE(pause_result.is_error);
        CHECK(harness.service.find("default", stored->id)->paused);

        harness.current_time = orangutan::automation::from_unix_seconds(1'200);
        auto resume_result = harness.invoke({{"op", "resume"}, {"id", stored->id}});
        CHECK_FALSE(resume_result.is_error);
        CHECK_FALSE(harness.service.find("default", stored->id)->paused);

        auto run_result = harness.invoke({{"op", "run"}, {"name", "repo-check"}});
        CHECK_FALSE(run_result.is_error);
        CHECK(harness.executed_ids.size() == 1UL);
        CHECK(harness.notifications.size() == 2UL);

        const auto runs = harness.invoke_json({{"op", "list_runs"}, {"id", stored->id}});
        REQUIRE(runs.is_array());
        REQUIRE(runs.size() == 1UL);
        CHECK(runs.at(0).at("delivery_status") == "notified");

        auto deliveries = harness.invoke_json({{"op", "list_deliveries"}, {"name", "repo-check"}, {"only_unacked", true}});
        REQUIRE(deliveries.is_array());
        REQUIRE(deliveries.size() == 2UL);

        const auto ack_result = harness.invoke({{"op", "ack_delivery"}, {"delivery_id", deliveries.at(0).at("id").get<std::string>()}});
        CHECK_FALSE(ack_result.is_error);

        deliveries = harness.invoke_json({{"op", "list_deliveries"}, {"name", "repo-check"}, {"only_unacked", true}});
        REQUIRE(deliveries.size() == 1UL);

        const auto clear_result = harness.invoke({{"op", "clear_deliveries"}, {"name", "repo-check"}});
        CHECK_FALSE(clear_result.is_error);

        deliveries = harness.invoke_json({{"op", "list_deliveries"}, {"name", "repo-check"}, {"only_unacked", true}});
        CHECK(deliveries.empty());
    };

    TEST_CASE("automation_tool_clear_deliveries_requires_agent_scope") {
        AutomationToolHarness harness("automation-tool-clear-scope.db");
        harness.context.agent_key.clear();

        const auto result = harness.invoke({{"op", "clear_deliveries"}});

        CHECK(result.content == "Error: agent_key is required.");
    };

} // namespace
