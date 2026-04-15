#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "automation/model.hpp"
#include "automation/parser.hpp"

namespace {

    TEST_CASE("cron_trigger_json_roundtrips_with_default_utc_time_zone") {
        const auto trigger = orangutan::automation::TriggerDefinition{
            .type = orangutan::automation::trigger_type::cron,
            .cron = "0 9 * * *",
        };

        const auto json = orangutan::automation::trigger_to_json(trigger);
        CHECK(json.at("type") == "cron");
        CHECK(json.at("cron") == "0 9 * * *");
        CHECK(json.at("time_zone") == "UTC");

        const auto parsed = orangutan::automation::trigger_from_json(json);
        REQUIRE(parsed.has_value());
        CHECK(parsed->type == orangutan::automation::trigger_type::cron);
        CHECK(parsed->cron == "0 9 * * *");
        CHECK(parsed->time_zone == "UTC");
    };

    TEST_CASE("once_trigger_json_uses_iso_utc_string") {
        const auto scheduled_at = orangutan::automation::from_unix_seconds(1'776'211'200);
        const auto trigger = orangutan::automation::TriggerDefinition{
            .type = orangutan::automation::trigger_type::once,
            .at = scheduled_at,
        };

        const auto json = orangutan::automation::trigger_to_json(trigger);
        CHECK(json.at("type") == "once");
        CHECK(json.at("at") == "2026-04-15T00:00:00Z");

        const auto parsed = orangutan::automation::trigger_from_json(json);
        REQUIRE(parsed.has_value());
        CHECK(parsed->type == orangutan::automation::trigger_type::once);
        CHECK(parsed->at == scheduled_at);
        CHECK(parsed->time_zone == "UTC");
    };

    TEST_CASE("trigger_from_json_defaults_interval_time_zone_to_utc_and_parses_active_windows") {
        const auto parsed = orangutan::automation::trigger_from_json({
            {"type", "interval"},
            {"every", "15m"},
            {"jitter", "30s"},
            {"active_windows", {
                {{"start", "09:00"}, {"end", "18:00"}},
                {{"start", "20:15"}, {"end", "21:45"}},
            }},
        });

        REQUIRE(parsed.has_value());
        CHECK(parsed->type == orangutan::automation::trigger_type::interval);
        CHECK(parsed->every == std::chrono::minutes{15});
        CHECK(parsed->jitter == std::chrono::seconds{30});
        CHECK(parsed->time_zone == "UTC");
        REQUIRE(parsed->active_windows.size() == 2UL);
        CHECK(parsed->active_windows.at(0).start == std::chrono::hours{9});
        CHECK(parsed->active_windows.at(0).end == std::chrono::hours{18});
        CHECK(parsed->active_windows.at(1).start == std::chrono::hours{20} + std::chrono::minutes{15});
        CHECK(parsed->active_windows.at(1).end == std::chrono::hours{21} + std::chrono::minutes{45});
    };

    TEST_CASE("trigger_from_json_requires_interval_jitter") {
        const auto parsed = orangutan::automation::trigger_from_json({
            {"type", "interval"},
            {"every", "15m"},
        });

        CHECK_FALSE(parsed.has_value());
        CHECK(parsed.error() == "jitter is required");
    };

    TEST_CASE("trigger_from_json_accepts_valid_iana_time_zones_for_cron_and_interval") {
        const auto cron_trigger = orangutan::automation::trigger_from_json({
            {"type", "cron"},
            {"cron", "0 9 * * *"},
            {"time_zone", "Etc/UTC"},
        });

        REQUIRE(cron_trigger.has_value());
        CHECK(cron_trigger->time_zone == "Etc/UTC");

        const auto interval_trigger = orangutan::automation::trigger_from_json({
            {"type", "interval"},
            {"every", "15m"},
            {"jitter", "30s"},
            {"time_zone", "Etc/UTC"},
        });

        REQUIRE(interval_trigger.has_value());
        CHECK(interval_trigger->time_zone == "Etc/UTC");
    };

    TEST_CASE("trigger_from_json_rejects_invalid_time_zone_names_for_cron_and_interval") {
        const auto cron_trigger = orangutan::automation::trigger_from_json({
            {"type", "cron"},
            {"cron", "0 9 * * *"},
            {"time_zone", "Mars/Olympus"},
        });

        CHECK_FALSE(cron_trigger.has_value());
        CHECK(cron_trigger.error() == "time_zone must be UTC or a valid IANA zone name");

        const auto interval_trigger = orangutan::automation::trigger_from_json({
            {"type", "interval"},
            {"every", "15m"},
            {"jitter", "30s"},
            {"time_zone", "Mars/Olympus"},
        });

        CHECK_FALSE(interval_trigger.has_value());
        CHECK(interval_trigger.error() == "time_zone must be UTC or a valid IANA zone name");
    };

    TEST_CASE("parse_duration_string_accepts_canonical_suffixes") {
        CHECK(orangutan::automation::parse_duration_string("45s").value() == std::chrono::seconds{45});
        CHECK(orangutan::automation::parse_duration_string("15m").value() == std::chrono::minutes{15});
        CHECK(orangutan::automation::parse_duration_string("2h").value() == std::chrono::hours{2});
    };

} // namespace
