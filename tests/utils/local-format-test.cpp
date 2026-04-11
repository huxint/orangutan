#include "utils/time-format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

using namespace orangutan;

namespace {

    std::string format_expected_local_date(std::chrono::system_clock::time_point tp) {
        const auto local_time = orangutan::time::local_time_from(tp);
        const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
        const auto ymd = std::chrono::year_month_day(local_day);

        std::ostringstream out;
        out << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-' << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
            << static_cast<unsigned>(ymd.day());
        return out.str();
    }

    std::string format_expected_local_timestamp(std::chrono::system_clock::time_point tp, char separator) {
        const auto truncated = std::chrono::floor<std::chrono::seconds>(tp);
        const auto local_time = orangutan::time::local_time_from(truncated);
        const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
        const auto ymd = std::chrono::year_month_day(local_day);
        const auto tod = std::chrono::hh_mm_ss(local_time - local_day);

        std::ostringstream out;
        out << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-' << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
            << static_cast<unsigned>(ymd.day()) << separator << std::setw(2) << tod.hours().count() << ':' << std::setw(2) << tod.minutes().count() << ':' << std::setw(2)
            << tod.seconds().count();
        return out.str();
    }

} // namespace

TEST_CASE("format_local_date_matches_local_calendar_values") {
    const auto tp = std::chrono::sys_days{std::chrono::year{2026} / 3 / 14} + std::chrono::hours{9} + std::chrono::minutes{30} + std::chrono::seconds{45};

    CHECK(orangutan::time::format_local_date(tp) == format_expected_local_date(tp));
};

TEST_CASE("format_local_timestamp_truncates_subseconds_and_matches_local_values") {
    const auto tp =
        std::chrono::sys_days{std::chrono::year{2026} / 3 / 14} + std::chrono::hours{9} + std::chrono::minutes{30} + std::chrono::seconds{45} + std::chrono::milliseconds{987};

    CHECK(orangutan::time::format_local_timestamp(tp) == format_expected_local_timestamp(tp, ' '));
};

TEST_CASE("format_local_iso8601_timestamp_uses_t_separator_and_matches_local_values") {
    const auto tp =
        std::chrono::sys_days{std::chrono::year{2026} / 3 / 14} + std::chrono::hours{9} + std::chrono::minutes{30} + std::chrono::seconds{45} + std::chrono::milliseconds{987};

    CHECK(orangutan::time::format_local_iso8601_timestamp(tp) == format_expected_local_timestamp(tp, 'T'));
};
