#include <chrono>
#include <memory>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "automation/builder.hpp"
#include "automation/model.hpp"
#include "automation/parser.hpp"
#include "test-helpers.hpp"

namespace {

    TEST_CASE("automation_builder_creates_cron_definition") {
        const auto result = orangutan::automation::Automation::named("repo-check")
                                .for_agent("default")
                                .run_prompt("scan repo and summarize changes")
                                .cron("0 9 * * *")
                                .time_zone("Asia/Shanghai")
                                .deliver_to("owner")
                                .tag("daily")
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.name == "repo-check");
        CHECK(automation.agent_key == "default");
        CHECK(automation.prompt == "scan repo and summarize changes");
        CHECK(automation.tags.size() == 1UL);
        CHECK(automation.tags.front() == "daily");
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::cron);
        CHECK(automation.trigger.cron == "0 9 * * *");
        CHECK(automation.trigger.time_zone == "Asia/Shanghai");
        CHECK(automation.delivery.mode == orangutan::automation::delivery_mode::notify);
        CHECK(automation.delivery.targets.size() == 1UL);
        CHECK(automation.delivery.targets.front() == "owner");
    };

    TEST_CASE("automation_builder_creates_interval_definition") {
        const auto result = orangutan::automation::Automation::named("pulse")
                                .for_agent("default")
                                .run_prompt("status check")
                                .every(std::chrono::minutes{15})
                                .jitter(std::chrono::seconds{30})
                                .within_hours({std::chrono::hours{9}, std::chrono::hours{18}})
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::interval);
        CHECK(automation.trigger.every == std::chrono::minutes{15});
        CHECK(automation.trigger.jitter == std::chrono::seconds{30});
        REQUIRE(automation.trigger.active_windows.size() == 1UL);
        CHECK(automation.trigger.active_windows.front().start == std::chrono::hours{9});
        CHECK(automation.trigger.active_windows.front().end == std::chrono::hours{18});
        CHECK(automation.trigger.time_zone == "UTC");
        CHECK(automation.delivery.mode == orangutan::automation::delivery_mode::silent);
    };

    TEST_CASE("interval_trigger_json_uses_duration_strings_and_time_zone") {
        const auto result = orangutan::automation::Automation::named("pulse")
                                .for_agent("default")
                                .run_prompt("status check")
                                .every(std::chrono::minutes{15})
                                .jitter(std::chrono::seconds{30})
                                .time_zone("UTC")
                                .within_hours({std::chrono::hours{9}, std::chrono::hours{18}})
                                .deliver_silently()
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        const auto json = orangutan::automation::trigger_to_json(automation.trigger);
        CHECK(json.at("type") == "interval");
        CHECK(json.at("every") == "15m");
        CHECK(json.at("jitter") == "30s");
        CHECK(json.at("time_zone") == "UTC");
        REQUIRE(json.at("active_windows").is_array());
        REQUIRE(json.at("active_windows").size() == 1UL);
        CHECK(json.at("active_windows").at(0).at("start") == "09:00");
        CHECK(json.at("active_windows").at(0).at("end") == "18:00");
    };

    TEST_CASE("automation_builder_creates_once_definition") {
        const auto scheduled_at = orangutan::automation::from_unix_seconds(1'776'249'600);

        const auto result = orangutan::automation::Automation::named("release-check")
                                .for_agent("default")
                                .run_prompt("prepare release summary")
                                .once_at(scheduled_at)
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::once);
        CHECK(automation.trigger.at == scheduled_at);
        CHECK(automation.trigger.time_zone == "UTC");
    };

    TEST_CASE("automation_builder_requires_agent_key_prompt_and_trigger") {
        {
            const auto result = orangutan::automation::Automation::named("missing-agent")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("agent key"));
        }

        {
            const auto result = orangutan::automation::Automation::named("missing-prompt")
                                    .for_agent("default")
                                    .cron("0 9 * * *")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("prompt"));
        }

        {
            const auto result = orangutan::automation::Automation::named("missing-trigger")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("trigger"));
        }
    };

    TEST_CASE("automation_builder_rejects_invalid_cron_expression") {
        std::ostringstream logged_output;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logged_output, false);
        orangutan::testing::ScopedDefaultLogger<spdlog::sinks::ostream_sink_mt> logger("automation-model-test", sink);

        const auto result = orangutan::automation::Automation::named("bad-cron")
                                .for_agent("default")
                                .run_prompt("check")
                                .cron("* * *")
                                .build();
        CHECK_FALSE(result.has_value());

        spdlog::default_logger()->flush();
        CHECK(logged_output.str().empty());
    };

    TEST_CASE("automation_builder_rejects_zero_or_negative_interval") {
        {
            const auto result = orangutan::automation::Automation::named("zero-interval")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .every(std::chrono::seconds{0})
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("cadence"));
        }

        {
            const auto result = orangutan::automation::Automation::named("negative-interval")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .every(std::chrono::seconds{-1})
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("cadence"));
        }
    };

    TEST_CASE("automation_builder_rejects_negative_jitter") {
        const auto result = orangutan::automation::Automation::named("negative-jitter")
                                .for_agent("default")
                                .run_prompt("check")
                                .every(std::chrono::minutes{15})
                                .jitter(std::chrono::seconds{-1})
                                .build();
        CHECK_FALSE(result.has_value());
        CHECK(result.error().contains("jitter"));
    };

    TEST_CASE("automation_builder_rejects_invalid_active_windows") {
        {
            const auto result = orangutan::automation::Automation::named("inverted-window")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .every(std::chrono::minutes{15})
                                    .within_hours({std::chrono::hours{18}, std::chrono::hours{9}})
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("active windows"));
        }

        {
            const auto result = orangutan::automation::Automation::named("out-of-range-window")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .every(std::chrono::minutes{15})
                                    .within_hours({std::chrono::hours{-1}, std::chrono::hours{9}})
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("active windows"));
        }
    };

    TEST_CASE("automation_builder_rejects_notify_delivery_with_empty_target") {
        const auto result = orangutan::automation::Automation::named("empty-target")
                                .for_agent("default")
                                .run_prompt("check")
                                .cron("0 9 * * *")
                                .deliver_to("")
                                .build();
        CHECK_FALSE(result.has_value());
        CHECK(result.error().contains("delivery target"));
    };

    TEST_CASE("automation_builder_rejects_empty_tag") {
        const auto result = orangutan::automation::Automation::named("empty-tag")
                                .for_agent("default")
                                .run_prompt("check")
                                .cron("0 9 * * *")
                                .tag("")
                                .build();
        CHECK_FALSE(result.has_value());
        CHECK(result.error().contains("tag"));
    };

    TEST_CASE("automation_builder_rejects_blank_required_strings") {
        {
            const auto result = orangutan::automation::Automation::named("   ")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("automation name"));
        }

        {
            const auto result = orangutan::automation::Automation::named("blank-agent")
                                    .for_agent(" \t ")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("agent key"));
        }

        {
            const auto result = orangutan::automation::Automation::named("blank-prompt")
                                    .for_agent("default")
                                    .run_prompt(" \n ")
                                    .cron("0 9 * * *")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("prompt"));
        }

        {
            const auto result = orangutan::automation::Automation::named("blank-target")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .deliver_to(" \t ")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("delivery target"));
        }

        {
            const auto result = orangutan::automation::Automation::named("blank-tag")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .tag(" \n ")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("tag"));
        }

        {
            const auto result = orangutan::automation::Automation::named("blank-time-zone")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .time_zone(" \t ")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("time zone"));
        }
    };

    TEST_CASE("automation_builder_rejects_invalid_time_zone_names_for_cron_and_interval") {
        {
            const auto result = orangutan::automation::Automation::named("bad-cron-zone")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .cron("0 9 * * *")
                                    .time_zone("Mars/Olympus")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("time_zone"));
        }

        {
            const auto result = orangutan::automation::Automation::named("bad-interval-zone")
                                    .for_agent("default")
                                    .run_prompt("check")
                                    .every(std::chrono::minutes{15})
                                    .jitter(std::chrono::seconds{30})
                                    .time_zone("Mars/Olympus")
                                    .build();
            CHECK_FALSE(result.has_value());
            CHECK(result.error().contains("time_zone"));
        }
    };

    TEST_CASE("automation_builder_once_trigger_only_supports_utc_time_zone") {
        const auto scheduled_at = orangutan::automation::from_unix_seconds(1'776'249'600);

        const auto result = orangutan::automation::Automation::named("once-zone")
                                .for_agent("default")
                                .run_prompt("check")
                                .once_at(scheduled_at)
                                .time_zone("Asia/Shanghai")
                                .build();
        CHECK_FALSE(result.has_value());
        CHECK(result.error().contains("UTC"));
    };

    TEST_CASE("automation_builder_resets_old_trigger_state_when_switching_to_cron") {
        const auto result = orangutan::automation::Automation::named("switch-to-cron")
                                .for_agent("default")
                                .run_prompt("check")
                                .every(std::chrono::minutes{15})
                                .jitter(std::chrono::seconds{30})
                                .within_hours({std::chrono::hours{9}, std::chrono::hours{18}})
                                .cron("0 9 * * *")
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::cron);
        CHECK(automation.trigger.cron == "0 9 * * *");
        CHECK(automation.trigger.every == std::chrono::seconds{0});
        CHECK(automation.trigger.jitter == std::chrono::seconds{0});
        CHECK(automation.trigger.active_windows.empty());
    };

    TEST_CASE("automation_builder_resets_old_trigger_state_when_switching_to_once") {
        const auto scheduled_at = orangutan::automation::from_unix_seconds(1'776'249'600);

        const auto result = orangutan::automation::Automation::named("switch-to-once")
                                .for_agent("default")
                                .run_prompt("check")
                                .cron("0 9 * * *")
                                .once_at(scheduled_at)
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::once);
        CHECK(automation.trigger.at == scheduled_at);
        CHECK(automation.trigger.cron.empty());
        CHECK(automation.trigger.every == std::chrono::seconds{0});
        CHECK(automation.trigger.jitter == std::chrono::seconds{0});
        CHECK(automation.trigger.active_windows.empty());
    };

    TEST_CASE("automation_builder_resets_old_time_zone_when_switching_to_once") {
        const auto scheduled_at = orangutan::automation::from_unix_seconds(1'776'249'600);

        const auto result = orangutan::automation::Automation::named("switch-once-zone")
                                .for_agent("default")
                                .run_prompt("check")
                                .cron("0 9 * * *")
                                .time_zone("Asia/Shanghai")
                                .once_at(scheduled_at)
                                .build();

        REQUIRE(result.has_value());
        const auto &automation = *result;
        CHECK(automation.trigger.type == orangutan::automation::trigger_type::once);
        CHECK(automation.trigger.at == scheduled_at);
        CHECK(automation.trigger.time_zone == "UTC");
    };

} // namespace
