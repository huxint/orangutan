#include "providers/http-client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("http_status_error_includes_status_and_body") {
    const auto error = orangutan::providers::detail::make_http_status_error(502, "upstream exploded");
    const std::string message = error.what();
    CHECK_THAT(message, ContainsSubstring("status 502"));
    CHECK_THAT(message, ContainsSubstring("upstream exploded"));
}

TEST_CASE("http_status_error_truncates_large_body") {
    std::string large_body(900, 'x');
    const auto error = orangutan::providers::detail::make_http_status_error(500, large_body);
    const std::string message = error.what();
    CHECK_THAT(message, ContainsSubstring("status 500"));
    CHECK_THAT(message, ContainsSubstring("truncated"));
}

