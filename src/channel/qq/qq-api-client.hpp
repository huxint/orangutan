#pragma once

#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    struct QqApiResponse {
        int http_status = 0;
        std::string body;
        std::string trace_id;
        std::string retry_after;
        int biz_code = 0;
        std::string biz_message;

        [[nodiscard]]
        nlohmann::json parse_json_body() const;
    };

    class QqApiClient {
    public:
        QqApiClient(std::string app_id, std::string client_secret);

        QqApiClient(const QqApiClient &) = delete;
        QqApiClient &operator=(const QqApiClient &) = delete;
        QqApiClient(QqApiClient &&) = delete;
        QqApiClient &operator=(QqApiClient &&) = delete;

        void ensure_access_token();
        void clear_access_token();

        [[nodiscard]]
        std::string access_token() const;

        [[nodiscard]]
        std::string get_gateway_url();

        [[nodiscard]]
        QqApiResponse get(const std::string &path);

        [[nodiscard]]
        QqApiResponse post(const std::string &path, const nlohmann::json &body);

        [[nodiscard]]
        QqApiResponse put(const std::string &path, const nlohmann::json &body);

        [[nodiscard]]
        static bool is_retryable_gateway_status(int status_code);

    private:
        struct HttpRawResponse {
            int status = 0;
            std::string body;
            std::string trace_id;
            std::string retry_after;
        };

        [[nodiscard]]
        QqApiResponse request_with_retry(std::string_view method, const std::string &path, const std::optional<nlohmann::json> &body);

        [[nodiscard]]
        HttpRawResponse perform_http_request(std::string_view method, const std::string &url, const std::optional<nlohmann::json> &body, bool with_auth);

        [[nodiscard]]
        static QqApiResponse normalize_response(const HttpRawResponse &raw);

        [[nodiscard]]
        static std::chrono::milliseconds parse_retry_after_delay(const std::string &retry_after);

        [[nodiscard]]
        static std::string build_api_error_message(std::string_view method, const std::string &path, const QqApiResponse &response);

        std::string app_id_;
        std::string client_secret_;

        mutable std::mutex token_mutex_;
        std::string access_token_;
        std::chrono::steady_clock::time_point token_expiry_{std::chrono::steady_clock::time_point::min()};
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    using channel::qq::QqApiClient;
    using channel::qq::QqApiResponse;

} // namespace orangutan
