#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "automation/builder.hpp"
#include "automation/repository.hpp"
#include "test-helpers.hpp"

namespace {

    [[nodiscard]]
    orangutan::automation::Automation make_cron_automation(std::string_view agent_key, std::string_view name) {
        return orangutan::automation::Automation::named(name)
            .for_agent(agent_key)
            .run_prompt("scan repo")
            .cron("0 9 * * *")
            .deliver_to("owner")
            .tag("daily")
            .build()
            .value();
    }

    [[nodiscard]]
    orangutan::automation::RunRecord make_run_record(const orangutan::automation::Automation &automation, std::int64_t started_at) {
        return orangutan::automation::RunRecord{
            .automation_id = automation.id,
            .agent_key = automation.agent_key,
            .automation_name = automation.name,
            .started_at = started_at,
            .status = "succeeded",
            .summary = "done",
        };
    }

    [[nodiscard]]
    orangutan::automation::DeliveryRecord make_delivery_record(const orangutan::automation::RunRecord &run, std::string_view target, std::int64_t created_at) {
        return orangutan::automation::DeliveryRecord{
            .run_id = run.id,
            .automation_id = run.automation_id,
            .agent_key = run.agent_key,
            .target = std::string(target),
            .status = "queued",
            .title = "repo-check",
            .body = "scan finished",
            .created_at = created_at,
        };
    }

    TEST_CASE("repository_roundtrips_automation_definitions_and_prefers_exact_id_matches") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-repository", "roundtrip.db");
        orangutan::automation::Repository repository(db_path);

        auto automation = make_cron_automation("default", "repo-check");
        const auto automation_id = repository.save(automation);
        automation.id = automation_id;

        auto colliding_name = make_cron_automation("default", automation_id);
        const auto colliding_id = repository.save(colliding_name);

        const auto by_id = repository.find("default", automation_id);
        REQUIRE(by_id.has_value());
        CHECK(by_id->id == automation_id);
        CHECK(by_id->name == "repo-check");

        const auto by_name = repository.find("default", "repo-check");
        REQUIRE(by_name.has_value());
        CHECK(by_name->id == automation_id);

        const auto list = repository.list(orangutan::automation::AutomationQuery{.agent_key = "default"});
        REQUIRE(list.size() == 2UL);
        CHECK(std::ranges::all_of(list, [](const orangutan::automation::Automation &entry) {
            return entry.agent_key == "default";
        }));
        CHECK(std::ranges::any_of(list, [colliding_id](const orangutan::automation::Automation &entry) {
            return entry.id == colliding_id;
        }));
    };

    TEST_CASE("repository_enforces_agent_scoped_unique_names") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-repository", "unique.db");
        orangutan::automation::Repository repository(db_path);

        static_cast<void>(repository.save(make_cron_automation("default", "repo-check")));
        CHECK_THROWS_AS(repository.save(make_cron_automation("default", "repo-check")), std::runtime_error);

        CHECK_NOTHROW(static_cast<void>(repository.save(make_cron_automation("ops", "repo-check"))));
    };

    TEST_CASE("repository_lists_runs_and_deliveries_with_agent_and_status_filters") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-repository", "history.db");
        orangutan::automation::Repository repository(db_path);

        auto alpha = make_cron_automation("default", "repo-check");
        alpha.id = repository.save(alpha);
        auto beta = make_cron_automation("ops", "repo-check");
        beta.id = repository.save(beta);

        auto alpha_run = make_run_record(alpha, 1'776'249'600);
        alpha_run.id = repository.insert_run(alpha_run);
        auto beta_run = make_run_record(beta, 1'776'249'660);
        beta_run.id = repository.insert_run(beta_run);

        auto alpha_delivery = make_delivery_record(alpha_run, "owner", 1'776'249'700);
        alpha_delivery.id = repository.insert_delivery(alpha_delivery);

        auto acked_delivery = make_delivery_record(alpha_run, "pager", 1'776'249'710);
        acked_delivery.id = repository.insert_delivery(acked_delivery);
        REQUIRE(repository.ack_delivery("default", acked_delivery.id).has_value());

        auto beta_delivery = make_delivery_record(beta_run, "ops", 1'776'249'720);
        static_cast<void>(repository.insert_delivery(beta_delivery));

        const auto default_runs = repository.list_runs(orangutan::automation::RunQuery{.agent_key = "default"});
        REQUIRE(default_runs.size() == 1UL);
        CHECK(default_runs.front().automation_id == alpha.id);

        const auto default_deliveries = repository.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default"});
        REQUIRE(default_deliveries.size() == 2UL);

        const auto unacked_deliveries =
            repository.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .only_unacked = true});
        REQUIRE(unacked_deliveries.size() == 1UL);
        CHECK(unacked_deliveries.front().target == "owner");
        CHECK_FALSE(unacked_deliveries.front().acked_at.has_value());
    };

    TEST_CASE("repository_clear_deliveries_requires_agent_scope_and_marks_rows_acked") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-repository", "clear.db");
        orangutan::automation::Repository repository(db_path);

        auto automation = make_cron_automation("default", "repo-check");
        automation.id = repository.save(automation);

        auto run = make_run_record(automation, 1'776'249'600);
        run.id = repository.insert_run(run);

        auto first_delivery = make_delivery_record(run, "owner", 1'776'249'700);
        first_delivery.id = repository.insert_delivery(first_delivery);
        auto second_delivery = make_delivery_record(run, "pager", 1'776'249'710);
        static_cast<void>(repository.insert_delivery(second_delivery));

        CHECK_THROWS_AS(repository.clear_deliveries(orangutan::automation::DeliveryQuery{}), std::invalid_argument);

        repository.clear_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default"});

        const auto deliveries = repository.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default"});
        REQUIRE(deliveries.size() == 2UL);
        CHECK(deliveries.at(0).acked_at.has_value());
        CHECK(deliveries.at(1).acked_at.has_value());
    };

} // namespace
