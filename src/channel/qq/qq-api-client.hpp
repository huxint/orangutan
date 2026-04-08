#pragma once

#include <nlohmann/json.hpp>
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
        virtual ~QqApiClient() = default;

        QqApiClient(const QqApiClient &) = delete;
        QqApiClient &operator=(const QqApiClient &) = delete;
        QqApiClient(QqApiClient &&) = delete;
        QqApiClient &operator=(QqApiClient &&) = delete;

        virtual void ensure_access_token();
        virtual void refresh_access_token_if_due();
        virtual void clear_access_token();

        [[nodiscard]]
        virtual std::string access_token() const;

        [[nodiscard]]
        virtual std::string get_gateway_url();

        [[nodiscard]]
        virtual QqApiResponse get(std::string_view path);

        [[nodiscard]]
        virtual QqApiResponse post(std::string_view path, const nlohmann::json &body);

        [[nodiscard]]
        virtual QqApiResponse put(std::string_view path, const nlohmann::json &body);

        [[nodiscard]]
        virtual QqApiResponse del(std::string_view path);

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
        QqApiResponse request_with_retry(std::string_view method, std::string_view path, const std::optional<nlohmann::json> &body);

        [[nodiscard]]
        HttpRawResponse perform_http_request(std::string_view method, std::string_view url, const std::optional<nlohmann::json> &body, bool with_auth) const;

        [[nodiscard]]
        static QqApiResponse normalize_response(HttpRawResponse raw);

        [[nodiscard]]
        static std::chrono::milliseconds parse_retry_after_delay(std::string_view retry_after);

        [[nodiscard]]
        static std::string build_api_error_message(std::string_view method, std::string_view path, const QqApiResponse &response);

        void refresh_access_token_locked(std::chrono::steady_clock::time_point now);

        std::string app_id_;
        std::string client_secret_;

        mutable std::mutex token_mutex_;
        std::string access_token_;
        std::chrono::steady_clock::time_point token_expiry_{std::chrono::steady_clock::time_point::min()};
        std::chrono::steady_clock::time_point token_background_refresh_at_{std::chrono::steady_clock::time_point::min()};
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    using channel::qq::QqApiClient;
    using channel::qq::QqApiResponse;

} // namespace orangutan
