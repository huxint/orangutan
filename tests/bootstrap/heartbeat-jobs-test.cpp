#include <algorithm>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "automation/builder.hpp"
#include "automation/repository.hpp"
#include "automation/service.hpp"
#include "bootstrap/heartbeat-jobs.hpp"
#include "config/config.hpp"
#include "heartbeat/heartbeat-automation.hpp"
#include "test-helpers.hpp"

namespace {

    struct HeartbeatJobsHarness {
        std::filesystem::path db_path = orangutan::testing::unique_test_db_path("heartbeat-jobs", "automation.db");
        orangutan::automation::Repository repository{db_path};
        orangutan::automation::AutomationService service{repository};
    };

    [[nodiscard]]
    bool has_tag(const orangutan::automation::Automation &automation, std::string_view tag) {
        return std::ranges::find(automation.tags, tag) != automation.tags.end();
    }

    TEST_CASE("reconcile_heartbeat_jobs_creates_managed_heartbeat_automations") {
        HeartbeatJobsHarness harness;
        orangutan::Config cfg;
        cfg.heartbeat_jobs.push_back({
            .name = "daily",
            .cron = "0 9 * * *",
            .prompt = "check repo health",
            .agent = "default",
            .channel = "cli",
        });

        orangutan::bootstrap::reconcile_heartbeat_jobs(cfg, harness.service);

        const auto automations = harness.service.list(orangutan::automation::AutomationQuery{.agent_key = "default"});
        REQUIRE(automations.size() == 1UL);
        const auto &automation = automations.front();
        CHECK(automation.name == "daily");
        CHECK(automation.prompt == "check repo health");
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::cron);
        CHECK(automation.trigger.cron == "0 9 * * *");
        CHECK(automation.delivery.mode == orangutan::automation::delivery_mode::notify);
        REQUIRE(automation.delivery.targets.size() == 1UL);
        CHECK(automation.delivery.targets.front() == "cli");
        CHECK(has_tag(automation, orangutan::heartbeat::HEARTBEAT_AUTOMATION_TAG));
        CHECK(has_tag(automation, orangutan::heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG));
    };

    TEST_CASE("reconcile_heartbeat_jobs_updates_existing_entries_and_removes_stale_managed_jobs") {
        HeartbeatJobsHarness harness;

        auto existing = orangutan::automation::Automation::named("daily")
                            .for_agent("default")
                            .run_prompt("old prompt")
                            .cron("0 8 * * *")
                            .deliver_to("cli")
                            .tag(orangutan::heartbeat::HEARTBEAT_AUTOMATION_TAG)
                            .tag(orangutan::heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG)
                            .build();
        existing.paused = true;
        existing.next_due_at = 1'234;
        const auto existing_id = harness.service.save(existing);

        static_cast<void>(harness.service.save(orangutan::automation::Automation::named("stale")
                                                   .for_agent("default")
                                                   .run_prompt("stale prompt")
                                                   .cron("0 6 * * *")
                                                   .deliver_to("cli")
                                                   .tag(orangutan::heartbeat::HEARTBEAT_AUTOMATION_TAG)
                                                   .tag(orangutan::heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG)
                                                   .build()));

        static_cast<void>(
            harness.service.save(orangutan::automation::Automation::named("manual").for_agent("default").run_prompt("manual prompt").cron("0 7 * * *").deliver_to("ops").build()));

        orangutan::Config cfg;
        cfg.heartbeat_jobs.push_back({
            .name = "daily",
            .cron = "0 9 * * *",
            .prompt = "new prompt",
            .agent = "default",
            .channel = "pager",
        });

        orangutan::bootstrap::reconcile_heartbeat_jobs(cfg, harness.service);

        const auto daily = harness.service.find("default", "daily");
        REQUIRE(daily.has_value());
        CHECK(daily->id == existing_id);
        CHECK(daily->prompt == "new prompt");
        REQUIRE(daily->delivery.targets.size() == 1UL);
        CHECK(daily->delivery.targets.front() == "pager");
        CHECK(daily->paused);
        REQUIRE(daily->next_due_at.has_value());
        CHECK(*daily->next_due_at == 1'234);

        CHECK_FALSE(harness.service.find("default", "stale").has_value());
        CHECK(harness.service.find("default", "manual").has_value());
    };

    TEST_CASE("reconcile_heartbeat_jobs_skips_duplicate_config_entries_for_the_same_agent_and_name") {
        HeartbeatJobsHarness harness;
        orangutan::Config cfg;
        cfg.heartbeat_jobs.push_back({
            .name = "daily",
            .cron = "0 9 * * *",
            .prompt = "first prompt",
            .agent = "default",
            .channel = "cli",
        });
        cfg.heartbeat_jobs.push_back({
            .name = "daily",
            .cron = "0 10 * * *",
            .prompt = "second prompt",
            .agent = "default",
            .channel = "pager",
        });

        orangutan::bootstrap::reconcile_heartbeat_jobs(cfg, harness.service);

        const auto automations = harness.service.list(orangutan::automation::AutomationQuery{.agent_key = "default"});
        REQUIRE(automations.size() == 1UL);
        CHECK(automations.front().prompt == "first prompt");
        REQUIRE(automations.front().delivery.targets.size() == 1UL);
        CHECK(automations.front().delivery.targets.front() == "cli");
    };

} // namespace
