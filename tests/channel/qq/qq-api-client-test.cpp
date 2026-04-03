#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#define private public
#include "channel/qq/qq-api-client.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    using PerformHttpRequestPtr = QqApiClient::HttpRawResponse (QqApiClient::*)(std::string_view method, const std::string &url, const std::optional<nlohmann::json> &body,
                                                                                bool with_auth) const;

    static_assert(std::is_same_v<decltype(&QqApiClient::perform_http_request), PerformHttpRequestPtr>);

    TEST_CASE("normalize_response_preserves_non_json_body_without_throwing") {
        const QqApiClient::HttpRawResponse raw{
            .status = 502,
            .body = "gateway temporarily unavailable",
            .trace_id = "trace-123",
            .retry_after = "1.5",
        };

        CHECK_NOTHROW([&] {
            const auto response = QqApiClient::normalize_response(raw);
            CHECK(response.http_status == 502);
            CHECK(response.body == raw.body);
            CHECK(response.trace_id == "trace-123");
            CHECK(response.retry_after == "1.5");
            CHECK(response.biz_code == 0);
            CHECK(response.biz_message.empty());
        }());
    };

} // namespace
