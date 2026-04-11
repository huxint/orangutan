#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "channel/qq/qq-api-client.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace orangutan::channel::qq::testing {

    struct QqApiClientTestAccess {
        using HttpRawResponse = QqApiClient::HttpRawResponse;

        [[nodiscard]]
        static QqApiResponse normalize_response(HttpRawResponse raw) {
            return QqApiClient::normalize_response(std::move(raw));
        }
    };

} // namespace orangutan::channel::qq::testing

namespace {

    TEST_CASE("normalize_response_preserves_non_json_body_without_throwing") {
        const orangutan::channel::qq::testing::QqApiClientTestAccess::HttpRawResponse raw{
            .status = 502,
            .body = "gateway temporarily unavailable",
            .trace_id = "trace-123",
            .retry_after = "1.5",
        };

        CHECK_NOTHROW([&] {
            const auto response = orangutan::channel::qq::testing::QqApiClientTestAccess::normalize_response(raw);
            CHECK(response.http_status == 502);
            CHECK(response.body == raw.body);
            CHECK(response.trace_id == "trace-123");
            CHECK(response.retry_after == "1.5");
            CHECK(response.biz_code == 0);
            CHECK(response.biz_message.empty());
        }());
    };

} // namespace
