#include "infra/time/local-format.hpp"

#include <regex>

#include "support/ut.hpp"

using namespace orangutan;

boost::ut::suite local_format_test_suite = [] {
    using namespace boost::ut;

    "current_local_date_matches_expected_shape"_test = [] {
        static const std::regex pattern{"^\\d{4}-\\d{2}-\\d{2}$"};
        expect(std::regex_match(orangutan::time::current_local_date(), pattern));
    };

    "current_local_timestamp_matches_expected_shape"_test = [] {
        static const std::regex pattern{"^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}$"};
        expect(std::regex_match(orangutan::time::current_local_timestamp(), pattern));
    };

    "current_local_iso8601_timestamp_matches_expected_shape"_test = [] {
        static const std::regex pattern{"^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}$"};
        expect(std::regex_match(orangutan::time::current_local_iso8601_timestamp(), pattern));
    };
};
