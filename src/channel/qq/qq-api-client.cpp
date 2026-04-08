#include "channel/qq/qq-api-client.hpp"

#include "providers/http-client.hpp"
#include "types/base.hpp"
#include "utils/string.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace orangutan::channel::qq {

    constexpr std::string_view QQ_API_BASE = "https://api.sgroup.qq.com";
    constexpr std::string_view QQ_TOKEN_URL = "https://bots.qq.com/app/getAppAccessToken";
    constexpr long DEFAULT_TIMEOUT_SECONDS = 120L;
    constexpr auto TOKEN_REFRESH_AHEAD = std::chrono::minutes(5);
    constexpr auto MIN_BACKGROUND_REFRESH_INTERVAL = std::chrono::seconds(60);

    [[nodiscard]]
    base::i64 parse_integer_like(const nlohmann::json &payload, std::string_view key, base::i64 default_value) {
        if (!payload.contains(key)) {
            return default_value;
        }

        const auto &value = payload.at(key);
        if (value.is_number_integer()) {
            return value.get<base::i64>();
        }
        if (value.is_string()) {
            std::string_view sv = value.get<std::string_view>();
            std::from_chars(sv.begin(), sv.end(), default_value);
        }
        return default_value;
    }

    [[nodiscard]]
    std::string to_lower_ascii(std::string_view input) {
        std::string lowered;
        lowered.reserve(input.size());
        for (char ch : input) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return lowered;
    }

    [[nodiscard]]
    bool is_absolute_url(std::string_view path) {
        return path.starts_with("https://") || path.starts_with("http://");
    }

    nlohmann::json QqApiResponse::parse_json_body() const {
        return body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
    }

    QqApiClient::QqApiClient(std::string app_id, std::string client_secret)
    : app_id_(std::move(app_id)),
      client_secret_(std::move(client_secret)) {}

    void QqApiClient::ensure_access_token() {
        std::scoped_lock lock(token_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!access_token_.empty() && now + TOKEN_REFRESH_AHEAD < token_expiry_) {
            return;
        }

        refresh_access_token_locked(now);
    }

    void QqApiClient::refresh_access_token_if_due() {
        std::scoped_lock lock(token_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!access_token_.empty() && now < token_background_refresh_at_) {
            return;
        }

        refresh_access_token_locked(now);
    }

    void QqApiClient::refresh_access_token_locked(std::chrono::steady_clock::time_point now) {
        const auto refresh_interval = [](std::chrono::seconds ttl) {
            return std::max(MIN_BACKGROUND_REFRESH_INTERVAL, ttl / 3);
        };

        const nlohmann::json request_body = {
            {"appId", app_id_},
            {"clientSecret", client_secret_},
        };

        const auto raw = perform_http_request("POST", std::string(QQ_TOKEN_URL), request_body, false);
        const auto response = normalize_response(raw);
        if (response.http_status >= 400) {
            throw std::runtime_error(build_api_error_message("POST", std::string(QQ_TOKEN_URL), response));
        }
        if (response.biz_code != 0) {
            throw std::runtime_error(build_api_error_message("POST", std::string(QQ_TOKEN_URL), response));
        }

        const auto payload = response.parse_json_body();
        if (!payload.contains("access_token") || !payload.at("access_token").is_string()) {
            throw std::runtime_error("QQ token response missing access_token");
        }

        access_token_ = payload.at("access_token").get<std::string>();
        const auto expires_in = std::chrono::seconds(std::max<base::i64>(60, parse_integer_like(payload, "expires_in", 7200)));
        token_expiry_ = now + expires_in;
        token_background_refresh_at_ = now + refresh_interval(expires_in);

        spdlog::debug("QQ access token refreshed successfully");
    }

    void QqApiClient::clear_access_token() {
        std::scoped_lock lock(token_mutex_);
        access_token_.clear();
        token_expiry_ = std::chrono::steady_clock::time_point::min();
        token_background_refresh_at_ = std::chrono::steady_clock::time_point::min();
    }

    std::string QqApiClient::access_token() const {
        std::scoped_lock lock(token_mutex_);
        return access_token_;
    }

    std::string QqApiClient::get_gateway_url() {
        const auto response = get("/gateway");
        const auto payload = response.parse_json_body();
        if (!payload.contains("url") || !payload.at("url").is_string()) {
            throw std::runtime_error("QQ gateway response missing url");
        }
        return payload.at("url").get<std::string>();
    }

    QqApiResponse QqApiClient::get(const std::string &path) {
        return request_with_retry("GET", path, std::nullopt);
    }

    QqApiResponse QqApiClient::post(const std::string &path, const nlohmann::json &body) {
        return request_with_retry("POST", path, body);
    }

    QqApiResponse QqApiClient::put(const std::string &path, const nlohmann::json &body) {
        return request_with_retry("PUT", path, body);
    }

    QqApiResponse QqApiClient::del(const std::string &path) {
        return request_with_retry("DELETE", path, std::nullopt);
    }

    bool QqApiClient::is_retryable_gateway_status(int status_code) {
        return status_code == 502 || status_code == 503 || status_code == 504;
    }

    QqApiResponse QqApiClient::request_with_retry(std::string_view method, const std::string &path, const std::optional<nlohmann::json> &body) {
        int token_refresh_retries = 0;
        int rate_limit_retries = 0;
        int gateway_retries = 0;

        while (true) {
            ensure_access_token();
            const auto full_url = is_absolute_url(path) ? path : std::string(QQ_API_BASE) + path;

            const auto raw = perform_http_request(method, full_url, body, true);
            auto response = normalize_response(raw);

            if (response.http_status == 401 && token_refresh_retries < 1) {
                ++token_refresh_retries;
                spdlog::warn("QQ API {} {} returned 401, refreshing token and retrying once", method, path);
                clear_access_token();
                continue;
            }

            if (response.http_status == 429 && rate_limit_retries < 2) {
                ++rate_limit_retries;
                const auto delay = parse_retry_after_delay(response.retry_after);
                spdlog::warn("QQ API {} {} rate limited (429), retrying after {}ms", method, path, delay.count());
                std::this_thread::sleep_for(delay);
                continue;
            }

            if (is_retryable_gateway_status(response.http_status) && gateway_retries < 2) {
                const auto delay = std::chrono::milliseconds(500 * (1 << gateway_retries));
                ++gateway_retries;
                spdlog::warn("QQ API {} {} temporary gateway failure ({}), retrying in {}ms", method, path, response.http_status, delay.count());
                std::this_thread::sleep_for(delay);
                continue;
            }

            if (response.http_status >= 400) {
                throw std::runtime_error(build_api_error_message(method, path, response));
            }
            if (response.biz_code != 0) {
                throw std::runtime_error(build_api_error_message(method, path, response));
            }

            return response;
        }
    }

    QqApiClient::HttpRawResponse QqApiClient::perform_http_request(std::string_view method, const std::string &url, const std::optional<nlohmann::json> &body,
                                                                   bool with_auth) const {
        CurlHandle curl;
        CurlHeaders headers;
        std::string response_body;
        std::string trace_id;
        std::string retry_after;
        std::string method_text(method);
        std::string body_text;
        std::array<char, CURL_ERROR_SIZE> error_buffer{};

        headers.append("Content-Type: application/json");
        headers.append("User-Agent: orangutan/qq-channel");
        if (with_auth) {
            headers.append("Authorization: QQBot " + access_token());
        }

        curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, DEFAULT_TIMEOUT_SECONDS);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, DEFAULT_TIMEOUT_SECONDS);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

        if (method == "GET") {
            curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
        } else {
            if (method == "POST") {
                curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            } else {
                curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method_text.c_str());
            }

            body_text = body.has_value() ? body->dump() : "{}";
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body_text.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body_text.size()));
        }

        auto write_callback = +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *buffer = static_cast<std::string *>(userdata);
            const auto total = size * nmemb;
            buffer->append(ptr, total);
            return total;
        };
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);

        struct HeaderCapture {
            std::string *trace_id = nullptr;
            std::string *retry_after = nullptr;
        } capture{
            .trace_id = &trace_id,
            .retry_after = &retry_after,
        };

        auto header_callback = +[](char *buffer, std::size_t size, std::size_t nitems, void *userdata) -> std::size_t {
            const auto total = size * nitems;
            if (userdata == nullptr || total == 0) {
                return total;
            }

            auto *capture = static_cast<HeaderCapture *>(userdata);
            std::string_view line(buffer, total);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                return total;
            }

            const auto key = to_lower_ascii(utils::trim_copy(line.substr(0, colon)));
            const auto value = static_cast<std::string>(utils::trim_copy(line.substr(colon + 1)));
            if (key == "x-tps-trace-id" && capture->trace_id != nullptr) {
                *capture->trace_id = value;
            } else if (key == "retry-after" && capture->retry_after != nullptr) {
                *capture->retry_after = value;
            }
            return total;
        };
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &capture);

        const auto code = curl_easy_perform(curl.get());
        if (code != CURLE_OK) {
            const std::string detail = error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code);
            throw std::runtime_error("QQ API request failed: " + detail);
        }

        long status_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);

        return HttpRawResponse{
            .status = static_cast<int>(status_code),
            .body = std::move(response_body),
            .trace_id = std::move(trace_id),
            .retry_after = std::move(retry_after),
        };
    }

    QqApiResponse QqApiClient::normalize_response(const HttpRawResponse &raw) {
        QqApiResponse normalized{
            .http_status = raw.status,
            .body = raw.body,
            .trace_id = raw.trace_id,
            .retry_after = raw.retry_after,
        };

        if (normalized.body.empty()) {
            return normalized;
        }

        if (const auto payload = nlohmann::json::parse(normalized.body, nullptr, false); !payload.is_discarded()) {
            if (payload.contains("code") && payload.at("code").is_number_integer()) {
                normalized.biz_code = payload.at("code").get<int>();
                if (payload.contains("message") && payload.at("message").is_string()) {
                    normalized.biz_message = payload.at("message").get<std::string>();
                }
            }
        }

        return normalized;
    }

    std::chrono::milliseconds QqApiClient::parse_retry_after_delay(const std::string &retry_after) {
        if (retry_after.empty()) {
            return std::chrono::seconds(1);
        }

        char *end_ptr = nullptr;
        const auto seconds = std::strtod(retry_after.c_str(), &end_ptr);
        if (end_ptr != retry_after.c_str() && std::isfinite(seconds) && seconds > 0.0) {
            return std::chrono::milliseconds(static_cast<base::i64>(seconds * 1000.0));
        }

        return std::chrono::seconds(1);
    }

    std::string QqApiClient::build_api_error_message(std::string_view method, const std::string &path, const QqApiResponse &response) {
        std::string message = "QQ API ";
        message.append(method);
        message.push_back(' ');
        message.append(path);
        message.append(" failed: http=");
        message.append(std::to_string(response.http_status));

        if (!response.trace_id.empty()) {
            message.append(", trace_id=");
            message.append(response.trace_id);
        }
        if (response.biz_code != 0) {
            message.append(", biz_code=");
            message.append(std::to_string(response.biz_code));
        }
        if (!response.biz_message.empty()) {
            message.append(", biz_message=");
            message.append(response.biz_message);
        } else if (!response.body.empty()) {
            message.append(", body=");
            message.append(response.body);
        }

        return message;
    }

} // namespace orangutan::channel::qq
