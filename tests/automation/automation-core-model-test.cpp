#include <chrono>
#include <type_traits>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "automation/action-registry.hpp"
#include "automation/core-model.hpp"

namespace {

    struct AgentPromptPayload {
        std::string agent;
        std::string prompt;
    };

    void to_json(nlohmann::json &json, const AgentPromptPayload &payload) {
        json = nlohmann::json{
            {"agent", payload.agent},
            {"prompt", payload.prompt},
        };
    }

    using orangutan::automation::ActionRegistry;
    using orangutan::automation::CronSchedule;
    using orangutan::automation::DispatchRequest;
    using orangutan::automation::ExecutionPolicy;
    using orangutan::automation::IntervalSchedule;
    using orangutan::automation::JobDefinition;
    using orangutan::automation::JobId;
    using orangutan::automation::OneShotSchedule;
    using orangutan::automation::OverlapPolicy;
    using orangutan::automation::ResultPolicy;
    using orangutan::automation::ScheduleSpec;
    using orangutan::automation::ScheduleState;

    TEST_CASE("core model uses schedule variants", "[automation][core-model]") {
        const ScheduleSpec cron_spec = CronSchedule{
            .expr = "0 * * * *",
            .time_zone = "UTC",
        };
        CHECK(std::holds_alternative<CronSchedule>(cron_spec));

        const ScheduleSpec interval_spec = IntervalSchedule{
            .every = std::chrono::minutes{15},
            .jitter = std::chrono::seconds{30},
        };
        CHECK(std::holds_alternative<IntervalSchedule>(interval_spec));

        const auto scheduled_at = std::chrono::system_clock::time_point{std::chrono::seconds{1'776'249'600}};
        const ScheduleSpec one_shot_spec = OneShotSchedule{
            .at = scheduled_at,
        };
        CHECK(std::holds_alternative<OneShotSchedule>(one_shot_spec));
    }

    TEST_CASE("core model strong ids are explicit wrappers", "[automation][core-model]") {
        STATIC_CHECK_FALSE(std::is_convertible_v<const char *, JobId>);

        const JobId job_id{.value = "job-1"};
        CHECK(job_id.value == "job-1");
    }

    TEST_CASE("core model execution policy has conservative defaults", "[automation][core-model]") {
        const ExecutionPolicy policy;

        CHECK(policy.max_retry_attempts == 0);
        CHECK(policy.initial_backoff == std::chrono::milliseconds{0});
        CHECK(policy.max_backoff == std::chrono::milliseconds{0});
        CHECK(policy.allow_parallel == false);
        CHECK(policy.overlap == OverlapPolicy::forbid);
    }

    TEST_CASE("core model schedule state defaults are idle", "[automation][core-model]") {
        const ScheduleState state;

        CHECK(state.enabled);
        CHECK_FALSE(state.paused);
        CHECK_FALSE(state.next_due_at.has_value());
        CHECK_FALSE(state.last_scheduled_at.has_value());
        CHECK_FALSE(state.last_started_at.has_value());
        CHECK_FALSE(state.last_finished_at.has_value());
        CHECK(state.last_status.empty());
        CHECK(state.in_flight_count == 0);
        CHECK(state.lease_owner.empty());
        CHECK_FALSE(state.lease_expires_at.has_value());
        CHECK(state.revision == 0);
    }

    TEST_CASE("core model aggregates compose job definitions", "[automation][core-model]") {
        const JobDefinition definition{
            .id = JobId{.value = "job-1"},
            .key = "repo-sync",
            .schedule =
                CronSchedule{
                    .expr = "0 * * * *",
                    .time_zone = "UTC",
                },
        };

        CHECK(definition.id.value == "job-1");
        CHECK(definition.key == "repo-sync");
        CHECK(std::holds_alternative<CronSchedule>(definition.schedule));

        const DispatchRequest request{
            .job_id = definition.id,
            .execution_id = {.value = "exec-1"},
        };
        CHECK(request.job_id.value == "job-1");
        CHECK(request.execution_id.value == "exec-1");

        const ResultPolicy result;
        CHECK(result.targets.empty());
    }

    TEST_CASE("action registry encodes typed payloads", "[automation][action]") {
        ActionRegistry registry;

        REQUIRE(registry.register_action<AgentPromptPayload>("agent.prompt").has_value());

        auto descriptor = registry.bind(
            "agent.prompt",
            AgentPromptPayload{
                .agent = "default",
                .prompt = "scan repo",
            });
        REQUIRE(descriptor.has_value());
        CHECK(descriptor->action_key == "agent.prompt");
        CHECK(descriptor->payload == nlohmann::json({
                                          {"agent", "default"},
                                          {"prompt", "scan repo"},
                                      }));
    }

    TEST_CASE("action registry rejects duplicate action keys", "[automation][action]") {
        ActionRegistry registry;

        REQUIRE(registry.register_action<AgentPromptPayload>("agent.prompt").has_value());

        auto duplicate = registry.register_action<AgentPromptPayload>("agent.prompt");
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(duplicate.error() == "action key already registered");
    }

    TEST_CASE("action registry rejects mismatched payload types", "[automation][action]") {
        ActionRegistry registry;
        REQUIRE(registry.register_action<AgentPromptPayload>("agent.prompt").has_value());

        auto descriptor = registry.bind("agent.prompt", std::string("not-a-payload"));
        REQUIRE_FALSE(descriptor.has_value());
        CHECK(descriptor.error() == "payload type does not match registered action");
    }

} // namespace
