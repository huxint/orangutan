#include <chrono>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "automation/dsl.hpp"

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

    struct FetchPayload {
        std::string branch;
    };

    void to_json(nlohmann::json &json, const FetchPayload &payload) {
        json = nlohmann::json{
            {"branch", payload.branch},
        };
    }

    struct NotifyPayload {
        std::string target;
        std::string summary;
    };

    void to_json(nlohmann::json &json, const NotifyPayload &payload) {
        json = nlohmann::json{
            {"target", payload.target},
            {"summary", payload.summary},
        };
    }

    TEST_CASE("dsl builds cron job definition with typed payload", "[automation][dsl]") {
        auto definition = orangutan::automation::job("repo-sync")
                              .schedule(orangutan::automation::cron("0 * * * *").time_zone("Asia/Shanghai"))
                              .missed_run(orangutan::automation::MissedRunPolicy::run_once)
                              .retry(3, std::chrono::seconds{1}, std::chrono::seconds{30})
                              .deliver_to("cli")
                              .bind("agent.prompt",
                                    AgentPromptPayload{
                                        .agent = "default",
                                        .prompt = "scan repo",
                                    })
                              .build();

        REQUIRE(definition.has_value());
        CHECK(definition->key == "repo-sync");
        CHECK(definition->action.action_key == "agent.prompt");
        CHECK(definition->action.payload == nlohmann::json({
                                                {"agent", "default"},
                                                {"prompt", "scan repo"},
                                            }));
        REQUIRE(std::holds_alternative<orangutan::automation::CronSchedule>(definition->schedule));
        const auto &schedule = std::get<orangutan::automation::CronSchedule>(definition->schedule);
        CHECK(schedule.expr == "0 * * * *");
        CHECK(schedule.time_zone == "Asia/Shanghai");
        CHECK(definition->execution.max_retry_attempts == 3);
        CHECK(definition->execution.initial_backoff == std::chrono::seconds{1});
        CHECK(definition->execution.max_backoff == std::chrono::seconds{30});
        CHECK(definition->result.mode == orangutan::automation::delivery_mode::notify);
        CHECK(definition->result.targets == std::vector<std::string>{"cli"});
    }

    TEST_CASE("dsl binds prebuilt pipeline descriptors into jobs", "[automation][dsl]") {
        auto action = orangutan::automation::pipeline()
                          .step("fetch.repo",
                                FetchPayload{
                                    .branch = "main",
                                })
                          .then("notify.owner",
                                NotifyPayload{
                                    .target = "cli",
                                    .summary = "done",
                                })
                          .build();
        REQUIRE(action.has_value());

        auto definition = orangutan::automation::job("pipeline-job").schedule(orangutan::automation::every(std::chrono::minutes{15})).bind(*action).build();

        REQUIRE(definition.has_value());
        CHECK(definition->action.action_key == "pipeline");
        REQUIRE(definition->action.payload.contains("steps"));
        REQUIRE(definition->action.payload["steps"].is_array());
        REQUIRE(definition->action.payload["steps"].size() == 2);
        CHECK(definition->action.payload["steps"][0]["action_key"] == "fetch.repo");
        CHECK(definition->action.payload["steps"][1]["payload"]["target"] == "cli");
    }

    TEST_CASE("dsl validates incomplete job definitions", "[automation][dsl]") {
        auto definition = orangutan::automation::job("repo-sync").schedule(orangutan::automation::cron("0 * * * *")).build();

        REQUIRE_FALSE(definition.has_value());
        CHECK(definition.error() == "action binding must be configured");
    }

    TEST_CASE("dsl validates empty pipelines", "[automation][dsl]") {
        auto action = orangutan::automation::pipeline().build();

        REQUIRE_FALSE(action.has_value());
        CHECK(action.error() == "pipeline must contain at least one step");
    }

    TEST_CASE("dsl rejects unsupported overlap policies", "[automation][dsl]") {
        auto definition = orangutan::automation::job("parallel-job")
                              .schedule(orangutan::automation::every(std::chrono::minutes{15}))
                              .overlap(orangutan::automation::OverlapPolicy::parallel)
                              .bind("agent.prompt",
                                    AgentPromptPayload{
                                        .agent = "default",
                                        .prompt = "scan repo",
                                    })
                              .build();

        REQUIRE_FALSE(definition.has_value());
        CHECK(definition.error() == "overlap policies other than forbid are not supported by the built-in driver");
    }

} // namespace
