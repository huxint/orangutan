#include "infra/time/local-time.hpp"

#include "support/ut.hpp"

#include <chrono>

using namespace std::chrono_literals;
using namespace orangutan;

boost::ut::suite local_time_test_suite = [] {
    using namespace boost::ut;

    "local_time_round_trips_with_chrono_types"_test = [] {
        const auto local = std::chrono::local_days{std::chrono::year{2026} / 3 / 14} + 9h + 30min + 45s;
        const auto tp = time::sys_time_from_local(local);
        const auto round_tripped = time::local_time_from(tp);

        expect(round_tripped == local);

        const auto local_day = std::chrono::floor<std::chrono::days>(round_tripped);
        const auto ymd = std::chrono::year_month_day{local_day};
        const auto tod = std::chrono::hh_mm_ss{round_tripped - local_day};

        expect(static_cast<int>(ymd.year()) == 2026_i);
        expect(static_cast<unsigned>(ymd.month()) == 3_u);
        expect(static_cast<unsigned>(ymd.day()) == 14_u);
        expect(tod.hours().count() == 9_i);
        expect(tod.minutes().count() == 30_i);
        expect(tod.seconds().count() == 45_i);
    };
};
