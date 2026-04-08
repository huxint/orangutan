#include "automation/cron-parser.hpp"
#include "utils/local-time.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    TEST_CASE("parses_wildcard_expression") {
        const auto expr = parse_cron("* * * * *");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.wildcard);
        CHECK(expr->hour.wildcard);
        CHECK(expr->day_of_month.wildcard);
        CHECK(expr->month.wildcard);
        CHECK(expr->day_of_week.wildcard);
    };

    TEST_CASE("parses_literal_values") {
        const auto expr = parse_cron("30 9 15 3 5");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.values.contains(30));
        CHECK(expr->hour.values.contains(9));
        CHECK(expr->day_of_month.values.contains(15));
        CHECK(expr->month.values.contains(3));
        CHECK(expr->day_of_week.values.contains(5));
    };

    TEST_CASE("parses_range_expression") {
        const auto expr = parse_cron("0 9 * * 1-5");
        REQUIRE(expr.has_value());
        CHECK_FALSE(expr->day_of_week.wildcard);
        CHECK(expr->day_of_week.values.size() == 5UL);
        CHECK(expr->day_of_week.values.contains(1));
        CHECK(expr->day_of_week.values.contains(5));
        CHECK_FALSE(expr->day_of_week.values.contains(0));
        CHECK_FALSE(expr->day_of_week.values.contains(6));
    };

    TEST_CASE("parses_step_expression") {
        const auto expr = parse_cron("*/15 * * * *");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.values.size() == 4UL);
        CHECK(expr->minute.values.contains(0));
        CHECK(expr->minute.values.contains(15));
        CHECK(expr->minute.values.contains(30));
        CHECK(expr->minute.values.contains(45));
    };

    TEST_CASE("parses_every_five_minutes") {
        const auto expr = parse_cron("*/5 * * * *");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.values.size() == 12UL);
        CHECK(expr->minute.values.contains(0));
        CHECK(expr->minute.values.contains(55));
    };

    TEST_CASE("rejects_invalid_field_count") {
        CHECK_FALSE(parse_cron("* * *").has_value());
        CHECK_FALSE(parse_cron("* * * * * *").has_value());
        CHECK_FALSE(parse_cron("").has_value());
    };

    TEST_CASE("rejects_invalid_field") {
        CHECK_FALSE(parse_cron("abc * * * *").has_value());
        CHECK_FALSE(parse_cron("* xyz * * *").has_value());
    };

    TEST_CASE("rejects_malformed_suffixes") {
        CHECK_FALSE(parse_cron("*/5foo * * * *").has_value());
    };

    TEST_CASE("rejects_out_of_range_values") {
        CHECK_FALSE(parse_cron("60 * * * *").has_value());
        CHECK_FALSE(parse_cron("* 24 * * *").has_value());
    };

    TEST_CASE("rejects_inverted_ranges") {
        CHECK_FALSE(parse_cron("10-5 * * * *").has_value());
    };

    TEST_CASE("rejects_non_positive_steps") {
        CHECK_FALSE(parse_cron("1-10/0 * * * *").has_value());
    };

    TEST_CASE("matches_wildcard") {
        const auto expr = parse_cron("* * * * *");
        REQUIRE(expr.has_value());
        CHECK(cron_matches(*expr, std::chrono::system_clock::now()));
    };

    TEST_CASE("matches_specific_time") {
        std::tm tm{};
        tm.tm_year = 126;
        tm.tm_mon = 2;
        tm.tm_mday = 14;
        tm.tm_hour = 9;
        tm.tm_min = 30;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        const auto time = std::chrono::system_clock::from_time_t(mktime(&tm));

        const auto expr = parse_cron("30 9 14 3 6");
        REQUIRE(expr.has_value());
        CHECK(cron_matches(*expr, time));

        const auto expr_no_match = parse_cron("0 10 14 3 6");
        REQUIRE(expr_no_match.has_value());
        CHECK_FALSE(cron_matches(*expr_no_match, time));
    };

    TEST_CASE("matches_day_of_month_or_day_of_week_when_both_restricted") {
        const auto expr = parse_cron("0 9 1 * 1");
        REQUIRE(expr.has_value());

        std::tm first_of_month_not_monday{};
        first_of_month_not_monday.tm_year = 126;
        first_of_month_not_monday.tm_mon = 3;
        first_of_month_not_monday.tm_mday = 1;
        first_of_month_not_monday.tm_hour = 9;
        first_of_month_not_monday.tm_min = 0;
        first_of_month_not_monday.tm_sec = 0;
        first_of_month_not_monday.tm_isdst = -1;
        const auto dom_match = std::chrono::system_clock::from_time_t(mktime(&first_of_month_not_monday));

        std::tm monday_not_first{};
        monday_not_first.tm_year = 126;
        monday_not_first.tm_mon = 3;
        monday_not_first.tm_mday = 6;
        monday_not_first.tm_hour = 9;
        monday_not_first.tm_min = 0;
        monday_not_first.tm_sec = 0;
        monday_not_first.tm_isdst = -1;
        const auto dow_match = std::chrono::system_clock::from_time_t(mktime(&monday_not_first));

        std::tm neither_match{};
        neither_match.tm_year = 126;
        neither_match.tm_mon = 3;
        neither_match.tm_mday = 8;
        neither_match.tm_hour = 9;
        neither_match.tm_min = 0;
        neither_match.tm_sec = 0;
        neither_match.tm_isdst = -1;
        const auto no_match = std::chrono::system_clock::from_time_t(mktime(&neither_match));

        CHECK(cron_matches(*expr, dom_match));
        CHECK(cron_matches(*expr, dow_match));
        CHECK_FALSE(cron_matches(*expr, no_match));
    };

    TEST_CASE("next_fire_time_finds_next_minute") {
        const auto expr = parse_cron("* * * * *");
        REQUIRE(expr.has_value());

        const auto now = std::chrono::system_clock::now();
        const auto next = next_fire_time(*expr, now);
        REQUIRE(next.has_value());
        CHECK(*next > now);

        const auto diff = std::chrono::duration_cast<std::chrono::seconds>(*next - now);
        CHECK(diff.count() <= 60);
    };

    TEST_CASE("next_fire_time_skips_non_matching_minutes") {
        const auto expr = parse_cron("0 * * * *");
        REQUIRE(expr.has_value());

        std::tm tm{};
        tm.tm_year = 126;
        tm.tm_mon = 2;
        tm.tm_mday = 14;
        tm.tm_hour = 9;
        tm.tm_min = 30;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        const auto time = std::chrono::system_clock::from_time_t(mktime(&tm));

        const auto next = next_fire_time(*expr, time);
        REQUIRE(next.has_value());

        const auto local_time = orangutan::time::local_time_from(*next);
        const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
        const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
        CHECK(tod.minutes().count() == 0);
        CHECK(tod.hours().count() == 10);
    };

    TEST_CASE("parses_comma_values") {
        const auto expr = parse_cron("0,30 * * * *");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.values.size() == 2UL);
        CHECK(expr->minute.values.contains(0));
        CHECK(expr->minute.values.contains(30));
    };

    TEST_CASE("parses_range_with_step") {
        const auto expr = parse_cron("1-30/10 * * * *");
        REQUIRE(expr.has_value());
        CHECK(expr->minute.values.size() == 3UL);
        CHECK(expr->minute.values.contains(1));
        CHECK(expr->minute.values.contains(11));
        CHECK(expr->minute.values.contains(21));
    };

} // namespace
