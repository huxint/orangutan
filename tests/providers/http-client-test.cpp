#include "providers/http-client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <unordered_map>
#include <vector>

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

TEST_CASE("compose_headers_applies_required_fallbacks_and_preserves_custom_headers") {
    const std::unordered_map<std::string, std::string> custom_headers{
        {"Authorization", "Bearer custom-token"},
        {"x-team", "infra"},
    };

    auto headers = orangutan::providers::compose_headers(custom_headers,
                                                         {orangutan::providers::HeaderFallback{.key = "Content-Type", .fallback = "application/json"},
                                                          orangutan::providers::HeaderFallback{.key = "Authorization", .fallback = "Bearer fallback-token"}});

    REQUIRE(headers.get() != nullptr);
    std::vector<std::string> values;
    for (const auto *item = headers.get(); item != nullptr; item = item->next) {
        values.emplace_back(item->data);
    }

    CHECK(values.size() == 3UL);
    CHECK(values[0] == "Content-Type: application/json");
    CHECK(values[1] == "Authorization: Bearer custom-token");
    CHECK(values[2] == "x-team: infra");
}
