#include "features/cron/parser.hpp"
#include "infra/time/local-time.hpp"
#include "support/ut.hpp"

using namespace orangutan;

namespace {

boost::ut::suite cron_parser_suite = [] {
    using namespace boost::ut;

    "parses_wildcard_expression"_test = [] {
        const auto expr = parse_cron("* * * * *");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.wildcard);
        expect(expr->hour.wildcard);
        expect(expr->day_of_month.wildcard);
        expect(expr->month.wildcard);
        expect(expr->day_of_week.wildcard);
    };

    "parses_literal_values"_test = [] {
        const auto expr = parse_cron("30 9 15 3 5");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.values.contains(30));
        expect(expr->hour.values.contains(9));
        expect(expr->day_of_month.values.contains(15));
        expect(expr->month.values.contains(3));
        expect(expr->day_of_week.values.contains(5));
    };

    "parses_range_expression"_test = [] {
        const auto expr = parse_cron("0 9 * * 1-5");
        expect(expr.has_value() >> fatal);
        expect(not expr->day_of_week.wildcard);
        expect(expr->day_of_week.values.size() == 5_ul);
        expect(expr->day_of_week.values.contains(1));
        expect(expr->day_of_week.values.contains(5));
        expect(not expr->day_of_week.values.contains(0));
        expect(not expr->day_of_week.values.contains(6));
    };

    "parses_step_expression"_test = [] {
        const auto expr = parse_cron("*/15 * * * *");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.values.size() == 4_ul);
        expect(expr->minute.values.contains(0));
        expect(expr->minute.values.contains(15));
        expect(expr->minute.values.contains(30));
        expect(expr->minute.values.contains(45));
    };

    "parses_every_five_minutes"_test = [] {
        const auto expr = parse_cron("*/5 * * * *");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.values.size() == 12_ul);
        expect(expr->minute.values.contains(0));
        expect(expr->minute.values.contains(55));
    };

    "rejects_invalid_field_count"_test = [] {
        expect(not parse_cron("* * *").has_value());
        expect(not parse_cron("* * * * * *").has_value());
        expect(not parse_cron("").has_value());
    };

    "rejects_invalid_field"_test = [] {
        expect(not parse_cron("abc * * * *").has_value());
        expect(not parse_cron("* xyz * * *").has_value());
    };

    "rejects_malformed_suffixes"_test = [] {
        expect(not parse_cron("*/5foo * * * *").has_value());
    };

    "rejects_out_of_range_values"_test = [] {
        expect(not parse_cron("60 * * * *").has_value());
        expect(not parse_cron("* 24 * * *").has_value());
    };

    "rejects_inverted_ranges"_test = [] {
        expect(not parse_cron("10-5 * * * *").has_value());
    };

    "rejects_non_positive_steps"_test = [] {
        expect(not parse_cron("1-10/0 * * * *").has_value());
    };

    "matches_wildcard"_test = [] {
        const auto expr = parse_cron("* * * * *");
        expect(expr.has_value() >> fatal);
        expect(cron_matches(*expr, std::chrono::system_clock::now()));
    };

    "matches_specific_time"_test = [] {
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
        expect(expr.has_value() >> fatal);
        expect(cron_matches(*expr, time));

        const auto expr_no_match = parse_cron("0 10 14 3 6");
        expect(expr_no_match.has_value() >> fatal);
        expect(not cron_matches(*expr_no_match, time));
    };

    "matches_day_of_month_or_day_of_week_when_both_restricted"_test = [] {
        const auto expr = parse_cron("0 9 1 * 1");
        expect(expr.has_value() >> fatal);

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

        expect(cron_matches(*expr, dom_match));
        expect(cron_matches(*expr, dow_match));
        expect(not cron_matches(*expr, no_match));
    };

    "next_fire_time_finds_next_minute"_test = [] {
        const auto expr = parse_cron("* * * * *");
        expect(expr.has_value() >> fatal);

        const auto now = std::chrono::system_clock::now();
        const auto next = next_fire_time(*expr, now);
        expect(next.has_value() >> fatal);
        expect(*next > now);

        const auto diff = std::chrono::duration_cast<std::chrono::seconds>(*next - now);
        expect(diff.count() <= 60_i);
    };

    "next_fire_time_skips_non_matching_minutes"_test = [] {
        const auto expr = parse_cron("0 * * * *");
        expect(expr.has_value() >> fatal);

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
        expect(next.has_value() >> fatal);

        const auto local_time = orangutan::time::local_time_from(*next);
        const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
        const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
        expect(tod.minutes().count() == 0_i);
        expect(tod.hours().count() == 10_i);
    };

    "parses_comma_values"_test = [] {
        const auto expr = parse_cron("0,30 * * * *");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.values.size() == 2_ul);
        expect(expr->minute.values.contains(0));
        expect(expr->minute.values.contains(30));
    };

    "parses_range_with_step"_test = [] {
        const auto expr = parse_cron("1-30/10 * * * *");
        expect(expr.has_value() >> fatal);
        expect(expr->minute.values.size() == 3_ul);
        expect(expr->minute.values.contains(1));
        expect(expr->minute.values.contains(11));
        expect(expr->minute.values.contains(21));
    };
};

} // namespace
