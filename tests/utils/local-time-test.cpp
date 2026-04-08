#include "utils/local-time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using namespace orangutan;

TEST_CASE("local_time_round_trips_with_chrono_types") {
    const auto local = std::chrono::local_days{std::chrono::year{2026} / 3 / 14} + 9h + 30min + 45s;
    const auto tp = time::sys_time_from_local(local);
    const auto round_tripped = time::local_time_from(tp);

    CHECK(round_tripped == local);

    const auto local_day = std::chrono::floor<std::chrono::days>(round_tripped);
    const auto ymd = std::chrono::year_month_day{local_day};
    const auto tod = std::chrono::hh_mm_ss{round_tripped - local_day};

    CHECK(static_cast<int>(ymd.year()) == 2026);
    CHECK(static_cast<unsigned>(ymd.month()) == 3U);
    CHECK(static_cast<unsigned>(ymd.day()) == 14U);
    CHECK(tod.hours().count() == 9);
    CHECK(tod.minutes().count() == 30);
    CHECK(tod.seconds().count() == 45);
};
