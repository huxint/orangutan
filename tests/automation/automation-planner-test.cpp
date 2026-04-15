#include <chrono>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "automation/builder.hpp"
#include "automation/model.hpp"
#include "automation/planner.hpp"

namespace {

    [[nodiscard]]
    orangutan::automation::Automation make_interval_automation(std::string_view name) {
        return orangutan::automation::Automation::named(name)
            .for_agent("default")
            .run_prompt("status check")
            .every(std::chrono::seconds{30})
            .build()
            .value();
    }

    [[nodiscard]]
    orangutan::automation::Automation make_once_automation(std::string_view name, orangutan::automation::TimePoint scheduled_at) {
        return orangutan::automation::Automation::named(name)
            .for_agent("default")
            .run_prompt("send summary")
            .once_at(scheduled_at)
            .build()
            .value();
    }

    [[nodiscard]]
    orangutan::automation::Automation make_cron_automation(std::string_view name, std::string_view expression, std::string_view time_zone) {
        return orangutan::automation::Automation::named(name)
            .for_agent("default")
            .run_prompt("scan repo")
            .cron(expression)
            .time_zone(time_zone)
            .build()
            .value();
    }

    TEST_CASE("interval_uses_fixed_delay_from_completion_time") {
        auto automation = make_interval_automation("pulse");
        automation.last_run_at = 100;
        automation.next_due_at = 130;

        const auto completion = orangutan::automation::from_unix_seconds(140);
        const auto next_due = orangutan::automation::plan_next_due(automation, completion);
        REQUIRE(next_due.has_value());
        CHECK(*next_due == 170);
    };

    TEST_CASE("interval_startup_planning_does_not_backfill_missed_cycles") {
        auto automation = make_interval_automation("restart-safe");
        automation.last_run_at = 100;
        automation.next_due_at.reset();

        const auto restart_time = orangutan::automation::from_unix_seconds(200);
        CHECK_FALSE(orangutan::automation::is_automation_due(automation, restart_time));

        const auto next_due = orangutan::automation::plan_next_due(automation, restart_time);
        REQUIRE(next_due.has_value());
        CHECK(*next_due == 230);
    };

    TEST_CASE("once_trigger_is_due_until_it_is_spent") {
        auto automation = make_once_automation("release-check", orangutan::automation::from_unix_seconds(1'000));
        automation.next_due_at.reset();

        CHECK_FALSE(orangutan::automation::is_automation_due(automation, orangutan::automation::from_unix_seconds(999)));
        CHECK(orangutan::automation::is_automation_due(automation, orangutan::automation::from_unix_seconds(1'000)));
        CHECK(orangutan::automation::is_automation_due(automation, orangutan::automation::from_unix_seconds(1'005)));

        const auto next_due = orangutan::automation::plan_next_due(automation, orangutan::automation::from_unix_seconds(900));
        REQUIRE(next_due.has_value());
        CHECK(*next_due == 1'000);

        automation.last_run_at = 1'005;
        CHECK_FALSE(orangutan::automation::is_automation_due(automation, orangutan::automation::from_unix_seconds(1'006)));
        CHECK_FALSE(orangutan::automation::plan_next_due(automation, orangutan::automation::from_unix_seconds(1'006)).has_value());
    };

    TEST_CASE("cron_planning_uses_trigger_time_zone") {
        auto automation = make_cron_automation("repo-check", "0 9 * * *", "Asia/Shanghai");

        const auto from = orangutan::automation::from_unix_seconds(1'776'189'600);
        const auto next_due = orangutan::automation::plan_next_due(automation, from);
        REQUIRE(next_due.has_value());
        CHECK(*next_due == 1'776'214'800);
    };

    TEST_CASE("interval_active_windows_clamp_to_next_open_window") {
        const auto result = orangutan::automation::Automation::named("windowed-pulse")
                                    .for_agent("default")
                                    .run_prompt("status check")
                                    .every(std::chrono::minutes{15})
                                    .time_zone("Asia/Shanghai")
                                    .within_hours({std::chrono::hours{9}, std::chrono::hours{18}})
                                    .build();
        REQUIRE(result.has_value());
        const auto &automation = *result;

        const auto completion = orangutan::automation::from_unix_seconds(1'776'247'500);
        const auto next_due = orangutan::automation::plan_next_due(automation, completion);
        REQUIRE(next_due.has_value());
        CHECK(*next_due == 1'776'301'200);
    };

    TEST_CASE("collect_due_automations_orders_due_items_and_uses_once_trigger_time") {
        auto once = make_once_automation("one-shot", orangutan::automation::from_unix_seconds(1'000));
        once.id = "auto-once";
        once.next_due_at.reset();

        auto interval = make_interval_automation("pulse");
        interval.id = "auto-interval";
        interval.next_due_at = 1'010;

        auto paused = make_once_automation("paused", orangutan::automation::from_unix_seconds(990));
        paused.id = "auto-paused";
        paused.paused = true;
        paused.next_due_at = 990;

        auto disabled = make_once_automation("disabled", orangutan::automation::from_unix_seconds(980));
        disabled.id = "auto-disabled";
        disabled.enabled = false;
        disabled.next_due_at = 980;

        const std::vector<orangutan::automation::Automation> automations{
            interval,
            paused,
            disabled,
            once,
        };

        const auto due = orangutan::automation::collect_due_automations(automations, orangutan::automation::from_unix_seconds(1'015));
        REQUIRE(due.size() == 2UL);
        CHECK(due.at(0).automation.id == "auto-once");
        CHECK(due.at(0).scheduled_for == 1'000);
        CHECK(due.at(1).automation.id == "auto-interval");
        CHECK(due.at(1).scheduled_for == 1'010);
    };

} // namespace
