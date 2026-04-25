#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "automation/builder.hpp"
#include "automation/repository.hpp"
#include "automation/runtime.hpp"
#include "automation/service.hpp"
#include "automation/sqlite-store.hpp"
#include "heartbeat/heartbeat-automation.hpp"
#include "test-helpers.hpp"
#include "utils/task-pool.hpp"

namespace {

    struct NotificationCall {
        std::string target;
        std::string title;
        std::string body;
    };

    struct ServiceHarness {
        std::filesystem::path db_path = orangutan::testing::unique_test_db_path("automation-service-runtime", "automation.db");
        orangutan::automation::Repository repository{db_path};
        orangutan::automation::TimePoint current_time = orangutan::automation::from_unix_seconds(1'000);
        orangutan::utils::TaskPool task_pool{1};
        orangutan::automation::AutomationService service{
            repository,
            [this] {
                return current_time;
            },
        };
        orangutan::automation::AutomationRuntime runtime{
            service,
            task_pool,
            [this] {
                return current_time;
            },
        };
        std::vector<std::string> executed_ids;
        std::vector<NotificationCall> notifications;

        ServiceHarness() {
            service.set_executor([this](const orangutan::automation::Automation &automation) {
                executed_ids.push_back(automation.id);
                return orangutan::automation::ExecutionResult{
                    .success = true,
                    .reply = "ok",
                    .summary = "ok",
                };
            });

            service.set_notifier([this](std::string_view target, std::string_view title, std::string_view body) -> std::optional<std::string> {
                notifications.push_back({
                    .target = std::string(target),
                    .title = std::string(title),
                    .body = std::string(body),
                });
                return std::nullopt;
            });
        }
    };

    [[nodiscard]]
    orangutan::automation::Automation make_once_automation(std::string_view name, orangutan::automation::TimePoint scheduled_at) {
        return orangutan::automation::Automation::named(name).for_agent("default").run_prompt("ship it").once_at(scheduled_at).build().value();
    }

    [[nodiscard]]
    orangutan::automation::Automation make_interval_automation(std::string_view name, std::chrono::seconds jitter = std::chrono::seconds{0}) {
        return orangutan::automation::Automation::named(name).for_agent("default").run_prompt("status check").every(std::chrono::seconds{30}).jitter(jitter).build().value();
    }

    [[nodiscard]]
    orangutan::automation::Automation make_notify_automation(std::string_view name) {
        return orangutan::automation::Automation::named(name)
            .for_agent("default")
            .run_prompt("scan repo")
            .cron("0 9 * * *")
            .deliver_to("owner")
            .deliver_to("pager")
            .build()
            .value();
    }

    [[nodiscard]]
    orangutan::automation::Automation make_managed_heartbeat_automation(std::string_view name, std::string_view target = "cli") {
        return orangutan::automation::Automation::named(name)
            .for_agent("default")
            .run_prompt("heartbeat check")
            .cron("0 9 * * *")
            .deliver_to(target)
            .tag(orangutan::heartbeat::HEARTBEAT_AUTOMATION_TAG)
            .tag(orangutan::heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG)
            .build()
            .value();
    }

    TEST_CASE("service_lists_finds_and_removes_automations") {
        ServiceHarness harness;

        const auto repo_check_id = harness.service.save(make_notify_automation("repo-check"));
        static_cast<void>(harness.service.save(orangutan::automation::Automation::named("ops-check").for_agent("ops").run_prompt("scan ops").cron("0 10 * * *").build().value()));

        const auto found = harness.service.find("default", repo_check_id);
        REQUIRE(found.has_value());
        CHECK(found->name == "repo-check");

        const auto listed = harness.service.list(orangutan::automation::AutomationQuery{.agent_key = "default"});
        REQUIRE(listed.size() == 1UL);
        CHECK(listed.front().id == repo_check_id);

        CHECK(harness.service.remove("default", repo_check_id));
        CHECK_FALSE(harness.service.find("default", repo_check_id).has_value());
    };

    TEST_CASE("service_scopes_core_job_keys_by_agent") {
        ServiceHarness harness;

        const auto default_id = harness.service.save(make_notify_automation("repo-check"));
        auto ops = make_notify_automation("repo-check");
        ops.agent_key = "ops";
        const auto ops_id = harness.service.save(ops);

        auto default_job = harness.service.find("default", default_id);
        auto ops_job = harness.service.find("ops", ops_id);
        REQUIRE(default_job.has_value());
        REQUIRE(ops_job.has_value());
        CHECK(default_job->name == "repo-check");
        CHECK(ops_job->name == "repo-check");
    };

    TEST_CASE("service_normalizes_disabled_state_and_resume_recomputes_next_due") {
        ServiceHarness harness;

        auto automation = make_interval_automation("pulse");
        const auto automation_id = harness.service.save(automation);

        auto stored = harness.service.find("default", automation_id);
        REQUIRE(stored.has_value());
        REQUIRE(stored->next_due_at.has_value());
        CHECK(*stored->next_due_at == 1'030);

        stored->enabled = false;
        stored->paused = true;
        stored->next_due_at = 1'030;
        static_cast<void>(harness.service.save(*stored));

        stored = harness.service.find("default", automation_id);
        REQUIRE(stored.has_value());
        CHECK_FALSE(stored->enabled);
        CHECK_FALSE(stored->paused);
        CHECK_FALSE(stored->next_due_at.has_value());

        stored->enabled = true;
        static_cast<void>(harness.service.save(*stored));
        CHECK(harness.service.pause("default", automation_id));

        harness.current_time = orangutan::automation::from_unix_seconds(1'200);
        CHECK(harness.service.resume("default", automation_id));

        stored = harness.service.find("default", automation_id);
        REQUIRE(stored.has_value());
        CHECK(stored->enabled);
        CHECK_FALSE(stored->paused);
        REQUIRE(stored->next_due_at.has_value());
        CHECK(*stored->next_due_at >= 1'230);
    };

    TEST_CASE("service_reads_schedule_state_from_core_store") {
        ServiceHarness harness;

        const auto automation_id = harness.service.save(make_interval_automation("state-owner"));
        auto stale_definition = harness.repository.find("default", automation_id);
        REQUIRE(stale_definition.has_value());
        stale_definition->enabled = false;
        stale_definition->paused = false;
        stale_definition->last_run_at = 999;
        stale_definition->next_due_at.reset();
        stale_definition->last_status = "stale";
        static_cast<void>(harness.repository.save(*stale_definition));

        const auto loaded = harness.service.find("default", automation_id);
        REQUIRE(loaded.has_value());
        CHECK(loaded->enabled);
        CHECK_FALSE(loaded->paused);
        CHECK_FALSE(loaded->last_run_at.has_value());
        REQUIRE(loaded->next_due_at.has_value());
        CHECK(*loaded->next_due_at == 1'030);
        CHECK(loaded->last_status.empty());

        CHECK(harness.service.pause("default", automation_id));
        const auto paused = harness.service.find("default", automation_id);
        REQUIRE(paused.has_value());
        CHECK(paused->paused);
    };

    TEST_CASE("service_save_preserves_runtime_only_core_state") {
        ServiceHarness harness;

        const auto automation_id = harness.service.save(make_interval_automation("preserve-runtime-state"));
        orangutan::automation::SqliteJobStore store(harness.db_path);
        auto loaded = store.load_job(orangutan::automation::JobId{.value = automation_id});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());

        auto stored = loaded->value();
        stored.state.in_flight_count = 1;
        stored.state.lease_owner = "driver-a";
        stored.state.lease_expires_at = 2'000;
        stored.state.last_started_at = 1'010;
        stored.state.revision = 42;
        REQUIRE(store.save_job(stored.definition, stored.state).has_value());

        auto automation = harness.service.find("default", automation_id);
        REQUIRE(automation.has_value());
        automation->notes = "updated";
        static_cast<void>(harness.service.save(*automation));

        loaded = store.load_job(orangutan::automation::JobId{.value = automation_id});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        CHECK(loaded->value().state.in_flight_count == 1);
        CHECK(loaded->value().state.lease_owner == "driver-a");
        REQUIRE(loaded->value().state.lease_expires_at.has_value());
        CHECK(*loaded->value().state.lease_expires_at == 2'000);
        REQUIRE(loaded->value().state.last_started_at.has_value());
        CHECK(*loaded->value().state.last_started_at == 1'010);
        CHECK(loaded->value().state.revision == 42);
    };

    TEST_CASE("service_run_now_executes_without_changing_disabled_state") {
        ServiceHarness harness;
        auto automation = make_once_automation("release-check", orangutan::automation::from_unix_seconds(900));
        automation.enabled = false;

        const auto automation_id = harness.service.save(automation);
        const auto run_id = harness.service.run_now("default", automation_id);

        CHECK_FALSE(run_id.empty());
        REQUIRE(harness.executed_ids.size() == 1UL);

        const auto stored = harness.service.find("default", automation_id);
        REQUIRE(stored.has_value());
        CHECK_FALSE(stored->enabled);
        REQUIRE(stored->last_run_at.has_value());
        CHECK(*stored->last_run_at == 1'000);
        CHECK_FALSE(stored->next_due_at.has_value());
    };

    TEST_CASE("service_records_silent_runs_without_creating_deliveries") {
        ServiceHarness harness;

        const auto automation_id = harness.service.save(make_once_automation("silent-once", orangutan::automation::from_unix_seconds(900)));
        static_cast<void>(harness.service.run_now("default", automation_id));

        const auto runs = harness.service.list_runs(orangutan::automation::RunQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(runs.size() == 1UL);
        CHECK(runs.front().delivery_status == "silent");

        const auto deliveries = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default"});
        CHECK(deliveries.empty());
    };

    TEST_CASE("service_records_notify_deliveries_and_supports_ack_and_clear") {
        ServiceHarness harness;

        const auto automation_id = harness.service.save(make_notify_automation("repo-check"));
        static_cast<void>(harness.service.run_now("default", automation_id));

        const auto runs = harness.service.list_runs(orangutan::automation::RunQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(runs.size() == 1UL);
        CHECK(runs.front().delivery_status == "notified");

        auto deliveries = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(deliveries.size() == 2UL);
        CHECK(harness.notifications.size() == 2UL);

        CHECK(harness.service.ack_delivery("default", deliveries.front().id));

        auto unacked = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id, .only_unacked = true});
        REQUIRE(unacked.size() == 1UL);

        CHECK_THROWS_AS(harness.service.clear_deliveries(orangutan::automation::DeliveryQuery{}), std::invalid_argument);

        harness.service.clear_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id});
        deliveries = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(deliveries.size() == 2UL);
        CHECK(deliveries.at(0).acked_at.has_value());
        CHECK(deliveries.at(1).acked_at.has_value());
    };

    TEST_CASE("service_persists_runs_when_notifier_throws") {
        ServiceHarness harness;
        harness.service.set_notifier([](std::string_view, std::string_view, std::string_view) -> std::optional<std::string> {
            throw std::runtime_error("notifier exploded");
        });

        const auto automation_id = harness.service.save(make_notify_automation("repo-check"));
        const auto run_id = harness.service.run_now("default", automation_id);

        CHECK_FALSE(run_id.empty());

        const auto runs = harness.service.list_runs(orangutan::automation::RunQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(runs.size() == 1UL);
        CHECK(runs.front().delivery_status == "notify_failed");

        const auto deliveries = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(deliveries.size() == 2UL);
        CHECK(std::ranges::all_of(deliveries, [](const orangutan::automation::DeliveryRecord &delivery) {
            return delivery.status == "notify_failed";
        }));

        const auto stored = harness.service.find("default", automation_id);
        REQUIRE(stored.has_value());
        REQUIRE(stored->last_run_at.has_value());
        CHECK(*stored->last_run_at == 1'000);
    };

    TEST_CASE("service_suppresses_heartbeat_ok_deliveries_for_managed_heartbeat_automations") {
        ServiceHarness harness;
        harness.service.add_delivery_filter([](const orangutan::automation::Automation &automation, const orangutan::automation::ExecutionResult &result) {
            return orangutan::heartbeat::heartbeat_delivery_disposition(automation, result, 300);
        });
        harness.service.set_executor([](const orangutan::automation::Automation &) {
            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "HEARTBEAT_OK all clear",
                .summary = "HEARTBEAT_OK all clear",
            };
        });

        const auto automation_id = harness.service.save(make_managed_heartbeat_automation("daily-heartbeat"));
        static_cast<void>(harness.service.run_now("default", automation_id));

        const auto runs = harness.service.list_runs(orangutan::automation::RunQuery{.agent_key = "default", .automation_id = automation_id});
        REQUIRE(runs.size() == 1UL);
        CHECK(runs.front().delivery_status == "heartbeat_ok");

        const auto deliveries = harness.service.list_deliveries(orangutan::automation::DeliveryQuery{.agent_key = "default", .automation_id = automation_id});
        CHECK(deliveries.empty());
        CHECK(harness.notifications.empty());
    };

    TEST_CASE("runtime_serializes_due_executions_per_agent") {
        ServiceHarness harness;
        const auto automation_id = harness.service.save(make_once_automation("lease-check", orangutan::automation::from_unix_seconds(900)));
        static_cast<void>(automation_id);

        std::promise<void> started;
        auto started_future = started.get_future();
        harness.service.set_executor([&started](const orangutan::automation::Automation &) {
            started.set_value();
            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "ok",
                .summary = "ok",
            };
        });

        std::optional<orangutan::automation::AutomationRuntime::AgentExecutionLease> lease;
        lease.emplace(harness.runtime.acquire_agent_execution_lease("default"));

        harness.runtime.start();

        CHECK(started_future.wait_for(std::chrono::milliseconds{100}) == std::future_status::timeout);
        lease.reset();
        CHECK(started_future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        harness.runtime.stop();
    };

    TEST_CASE("runtime_start_preserves_future_interval_due_across_restart") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-service-runtime", "restart.db");
        orangutan::automation::TimePoint current_time = orangutan::automation::from_unix_seconds(1'000);

        orangutan::automation::Repository repository(db_path);
        orangutan::automation::AutomationService service(repository, [&current_time] {
            return current_time;
        });
        const auto automation_id = service.save(make_interval_automation("restart-safe", std::chrono::seconds{10}));

        const auto initial = service.find("default", automation_id);
        REQUIRE(initial.has_value());
        REQUIRE(initial->next_due_at.has_value());
        const auto initial_due = *initial->next_due_at;

        orangutan::automation::Repository reopened_repository(db_path);
        orangutan::automation::AutomationService reopened_service(reopened_repository, [&current_time] {
            return current_time;
        });
        orangutan::utils::TaskPool reopened_pool{1};
        orangutan::automation::AutomationRuntime reopened_runtime(reopened_service, reopened_pool, [&current_time] {
            return current_time;
        });

        reopened_runtime.start();
        reopened_runtime.stop();

        const auto reopened = reopened_service.find("default", automation_id);
        REQUIRE(reopened.has_value());
        REQUIRE(reopened->next_due_at.has_value());
        CHECK(*reopened->next_due_at == initial_due);
    };

    TEST_CASE("runtime_start_dispatches_due_jobs_from_persisted_schedule_state") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-service-runtime", "persisted-due.db");
        orangutan::automation::TimePoint current_time = orangutan::automation::from_unix_seconds(1'000);

        {
            orangutan::automation::Repository repository(db_path);
            orangutan::automation::AutomationService service(repository, [&current_time] {
                return current_time;
            });
            static_cast<void>(service.save(make_once_automation("persisted-due", orangutan::automation::from_unix_seconds(900))));
        }

        orangutan::automation::Repository reopened_repository(db_path);
        orangutan::automation::AutomationService reopened_service(reopened_repository, [&current_time] {
            return current_time;
        });
        orangutan::utils::TaskPool reopened_pool{1};
        orangutan::automation::AutomationRuntime reopened_runtime(reopened_service, reopened_pool, [&current_time] {
            return current_time;
        });

        std::promise<void> executed;
        auto executed_future = executed.get_future();
        reopened_service.set_executor([&executed](const orangutan::automation::Automation &) {
            executed.set_value();
            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "ok",
                .summary = "ok",
            };
        });

        reopened_runtime.start();
        REQUIRE(executed_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        reopened_runtime.stop();
    };

    TEST_CASE("runtime_wakes_when_a_due_job_is_saved_after_start") {
        ServiceHarness harness;

        std::promise<void> executed;
        auto executed_future = executed.get_future();
        harness.service.set_executor([&executed](const orangutan::automation::Automation &) {
            executed.set_value();
            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "ok",
                .summary = "ok",
            };
        });

        harness.runtime.start();
        static_cast<void>(harness.service.save(make_once_automation("saved-after-start", orangutan::automation::from_unix_seconds(900))));

        REQUIRE(executed_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        harness.runtime.stop();
    };

    TEST_CASE("runtime_start_leaves_idle_systems_parked_without_tick_polling") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-service-runtime", "idle-start.db");
        orangutan::automation::Repository repository(db_path);

        std::atomic<int> clock_calls = 0;
        const auto fixed_time = orangutan::automation::from_unix_seconds(1'000);
        orangutan::automation::AutomationService service(repository, [fixed_time] {
            return fixed_time;
        });
        orangutan::utils::TaskPool task_pool{1};
        orangutan::automation::AutomationRuntime runtime(service, task_pool, [&] {
            clock_calls.fetch_add(1, std::memory_order_relaxed);
            return fixed_time;
        });

        runtime.start();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (clock_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        REQUIRE(clock_calls.load() >= 1);

        const auto calls_after_start = clock_calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        const auto calls_after_window = clock_calls.load();

        runtime.stop();

        CHECK(calls_after_window == calls_after_start);
    };

    TEST_CASE("runtime_can_restart_the_same_instance_after_stop") {
        ServiceHarness harness;

        std::promise<void> first_run;
        std::promise<void> second_run;
        auto first_run_future = first_run.get_future();
        auto second_run_future = second_run.get_future();
        std::atomic<int> execution_count = 0;

        harness.service.set_executor([&](const orangutan::automation::Automation &) {
            const auto current_count = execution_count.fetch_add(1) + 1;
            if (current_count == 1) {
                first_run.set_value();
            } else if (current_count == 2) {
                second_run.set_value();
            }

            return orangutan::automation::ExecutionResult{
                .success = true,
                .reply = "ok",
                .summary = "ok",
            };
        });

        static_cast<void>(harness.service.save(make_once_automation("first-run", orangutan::automation::from_unix_seconds(900))));
        harness.runtime.start();
        REQUIRE(first_run_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        harness.runtime.stop();

        static_cast<void>(harness.service.save(make_once_automation("second-run", orangutan::automation::from_unix_seconds(900))));
        harness.runtime.start();
        REQUIRE(second_run_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        harness.runtime.stop();

        CHECK(execution_count.load() == 2);
    };

} // namespace
