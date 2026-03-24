#include "infra/time/local-format.hpp"

#include <ctre.hpp>

#include "support/ut.hpp"

using namespace orangutan;

boost::ut::suite local_format_test_suite = [] {
    using namespace boost::ut;

    "current_local_date_matches_expected_shape"_test = [] {
        expect(ctre::match<R"(\d{4}-\d{2}-\d{2})">(orangutan::time::current_local_date()).operator bool());
    };

    "current_local_timestamp_matches_expected_shape"_test = [] {
        expect(ctre::match<R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})">(orangutan::time::current_local_timestamp()).operator bool());
    };

    "current_local_iso8601_timestamp_matches_expected_shape"_test = [] {
        expect(ctre::match<R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})">(orangutan::time::current_local_iso8601_timestamp()).operator bool());
    };
};
