#include "utils/time-format.hpp"

#include <ctre.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

TEST_CASE("current_local_date_matches_expected_shape") {
    CHECK(ctre::match<R"(\d{4}-\d{2}-\d{2})">(orangutan::time::current_local_date()).operator bool());
};

TEST_CASE("current_local_timestamp_matches_expected_shape") {
    CHECK(ctre::match<R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})">(orangutan::time::current_local_timestamp()).operator bool());
};

TEST_CASE("current_local_iso8601_timestamp_matches_expected_shape") {
    CHECK(ctre::match<R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})">(orangutan::time::current_local_iso8601_timestamp()).operator bool());
};
