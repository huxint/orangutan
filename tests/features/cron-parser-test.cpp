#include "features/cron/parser.hpp"

#include <gtest/gtest.h>

using namespace orangutan;

TEST(CronParserTest, ParsesWildcardExpression) {
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_TRUE(expr->minute.wildcard);
    EXPECT_TRUE(expr->hour.wildcard);
    EXPECT_TRUE(expr->day_of_month.wildcard);
    EXPECT_TRUE(expr->month.wildcard);
    EXPECT_TRUE(expr->day_of_week.wildcard);
}

TEST(CronParserTest, ParsesLiteralValues) {
    auto expr = parse_cron("30 9 15 3 5");
    ASSERT_TRUE(expr.has_value());
    EXPECT_TRUE(expr->minute.values.contains(30));
    EXPECT_TRUE(expr->hour.values.contains(9));
    EXPECT_TRUE(expr->day_of_month.values.contains(15));
    EXPECT_TRUE(expr->month.values.contains(3));
    EXPECT_TRUE(expr->day_of_week.values.contains(5));
}

TEST(CronParserTest, ParsesRangeExpression) {
    auto expr = parse_cron("0 9 * * 1-5");
    ASSERT_TRUE(expr.has_value());
    EXPECT_FALSE(expr->day_of_week.wildcard);
    EXPECT_EQ(expr->day_of_week.values.size(), 5);
    EXPECT_TRUE(expr->day_of_week.values.contains(1));
    EXPECT_TRUE(expr->day_of_week.values.contains(5));
    EXPECT_FALSE(expr->day_of_week.values.contains(0));
    EXPECT_FALSE(expr->day_of_week.values.contains(6));
}

TEST(CronParserTest, ParsesStepExpression) {
    auto expr = parse_cron("*/15 * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_EQ(expr->minute.values.size(), 4);
    EXPECT_TRUE(expr->minute.values.contains(0));
    EXPECT_TRUE(expr->minute.values.contains(15));
    EXPECT_TRUE(expr->minute.values.contains(30));
    EXPECT_TRUE(expr->minute.values.contains(45));
}

TEST(CronParserTest, ParsesEveryFiveMinutes) {
    auto expr = parse_cron("*/5 * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_EQ(expr->minute.values.size(), 12);
    EXPECT_TRUE(expr->minute.values.contains(0));
    EXPECT_TRUE(expr->minute.values.contains(55));
}

TEST(CronParserTest, RejectsInvalidFieldCount) {
    EXPECT_FALSE(parse_cron("* * *").has_value());
    EXPECT_FALSE(parse_cron("* * * * * *").has_value());
    EXPECT_FALSE(parse_cron("").has_value());
}

TEST(CronParserTest, RejectsInvalidField) {
    EXPECT_FALSE(parse_cron("abc * * * *").has_value());
    EXPECT_FALSE(parse_cron("* xyz * * *").has_value());
}

TEST(CronParserTest, RejectsMalformedSuffixes) {
    EXPECT_FALSE(parse_cron("*/5foo * * * *").has_value());
}

TEST(CronParserTest, RejectsOutOfRangeValues) {
    EXPECT_FALSE(parse_cron("60 * * * *").has_value());
    EXPECT_FALSE(parse_cron("* 24 * * *").has_value());
}

TEST(CronParserTest, RejectsInvertedRanges) {
    EXPECT_FALSE(parse_cron("10-5 * * * *").has_value());
}

TEST(CronParserTest, RejectsNonPositiveSteps) {
    EXPECT_FALSE(parse_cron("1-10/0 * * * *").has_value());
}

TEST(CronParserTest, MatchesWildcard) {
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_TRUE(cron_matches(*expr, std::chrono::system_clock::now()));
}

TEST(CronParserTest, MatchesSpecificTime) {
    // Build a specific time: 2026-03-14 09:30 (Saturday = day 6)
    std::tm tm{};
    tm.tm_year = 126; // 2026
    tm.tm_mon = 2;    // March
    tm.tm_mday = 14;
    tm.tm_hour = 9;
    tm.tm_min = 30;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    auto time = std::chrono::system_clock::from_time_t(mktime(&tm));

    auto expr = parse_cron("30 9 14 3 6");
    ASSERT_TRUE(expr.has_value());
    EXPECT_TRUE(cron_matches(*expr, time));

    auto expr_no_match = parse_cron("0 10 14 3 6");
    ASSERT_TRUE(expr_no_match.has_value());
    EXPECT_FALSE(cron_matches(*expr_no_match, time));
}

TEST(CronParserTest, MatchesDayOfMonthOrDayOfWeekWhenBothRestricted) {
    auto expr = parse_cron("0 9 1 * 1");
    ASSERT_TRUE(expr.has_value());

    std::tm first_of_month_not_monday{};
    first_of_month_not_monday.tm_year = 126; // 2026
    first_of_month_not_monday.tm_mon = 3;    // April
    first_of_month_not_monday.tm_mday = 1;   // Wednesday
    first_of_month_not_monday.tm_hour = 9;
    first_of_month_not_monday.tm_min = 0;
    first_of_month_not_monday.tm_sec = 0;
    first_of_month_not_monday.tm_isdst = -1;
    auto dom_match = std::chrono::system_clock::from_time_t(mktime(&first_of_month_not_monday));

    std::tm monday_not_first{};
    monday_not_first.tm_year = 126; // 2026
    monday_not_first.tm_mon = 3;    // April
    monday_not_first.tm_mday = 6;   // Monday
    monday_not_first.tm_hour = 9;
    monday_not_first.tm_min = 0;
    monday_not_first.tm_sec = 0;
    monday_not_first.tm_isdst = -1;
    auto dow_match = std::chrono::system_clock::from_time_t(mktime(&monday_not_first));

    std::tm neither_match{};
    neither_match.tm_year = 126; // 2026
    neither_match.tm_mon = 3;    // April
    neither_match.tm_mday = 8;   // Wednesday
    neither_match.tm_hour = 9;
    neither_match.tm_min = 0;
    neither_match.tm_sec = 0;
    neither_match.tm_isdst = -1;
    auto no_match = std::chrono::system_clock::from_time_t(mktime(&neither_match));

    EXPECT_TRUE(cron_matches(*expr, dom_match));
    EXPECT_TRUE(cron_matches(*expr, dow_match));
    EXPECT_FALSE(cron_matches(*expr, no_match));
}

TEST(CronParserTest, NextFireTimeFindsNextMinute) {
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());

    auto now = std::chrono::system_clock::now();
    auto next = next_fire_time(*expr, now);
    ASSERT_TRUE(next.has_value());
    EXPECT_GT(*next, now);

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(*next - now);
    EXPECT_LE(diff.count(), 60);
}

TEST(CronParserTest, NextFireTimeSkipsNonMatchingMinutes) {
    // Every hour at :00
    auto expr = parse_cron("0 * * * *");
    ASSERT_TRUE(expr.has_value());

    // Set time to xx:30
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 2;
    tm.tm_mday = 14;
    tm.tm_hour = 9;
    tm.tm_min = 30;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    auto time = std::chrono::system_clock::from_time_t(mktime(&tm));

    auto next = next_fire_time(*expr, time);
    ASSERT_TRUE(next.has_value());

    auto next_time = std::chrono::system_clock::to_time_t(*next);
    std::tm result{};
    localtime_r(&next_time, &result);
    EXPECT_EQ(result.tm_min, 0);
    EXPECT_EQ(result.tm_hour, 10);
}

TEST(CronParserTest, ParsesCommaValues) {
    auto expr = parse_cron("0,30 * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_EQ(expr->minute.values.size(), 2);
    EXPECT_TRUE(expr->minute.values.contains(0));
    EXPECT_TRUE(expr->minute.values.contains(30));
}

TEST(CronParserTest, ParsesRangeWithStep) {
    auto expr = parse_cron("1-30/10 * * * *");
    ASSERT_TRUE(expr.has_value());
    EXPECT_EQ(expr->minute.values.size(), 3);
    EXPECT_TRUE(expr->minute.values.contains(1));
    EXPECT_TRUE(expr->minute.values.contains(11));
    EXPECT_TRUE(expr->minute.values.contains(21));
}
