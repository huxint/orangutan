#pragma once

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include "utils/transparent-lookup.hpp"

namespace orangutan::providers {

    namespace detail {

        inline std::string truncate_http_error_body(std::string_view body, std::size_t max_chars = 512) {
            if (body.empty()) {
                return {};
            }

            if (body.size() <= max_chars) {
                return std::string(body);
            }

            std::string truncated(body.substr(0, max_chars));
            truncated += "... (truncated)";
            return truncated;
        }

        inline std::runtime_error make_http_status_error(long http_code, std::string_view body) {
            std::string message = "HTTP request failed with status " + std::to_string(http_code);
            const auto snippet = truncate_http_error_body(body);
            if (!snippet.empty()) {
                message += ": " + snippet;
            }
            return std::runtime_error(message);
        }

    } // namespace detail

    // RAII wrapper for libcurl easy handle
    class CurlHandle {
    public:
        CurlHandle()
        : handle_(curl_easy_init()) {
            if (handle_ == nullptr) {
                throw std::runtime_error("Failed to initialize libcurl");
            }
        }

        ~CurlHandle() {
            if (handle_ != nullptr) {
                curl_easy_cleanup(handle_);
            }
        }

        CurlHandle(const CurlHandle &) = delete;
        CurlHandle &operator=(const CurlHandle &) = delete;
        CurlHandle(CurlHandle &&) = delete;
        CurlHandle &operator=(CurlHandle &&) = delete;

        [[nodiscard]]
        CURL *get() const {
            return handle_;
        }

    private:
        CURL *handle_;
    };

    // RAII wrapper for curl_slist
    class CurlHeaders {
    public:
        CurlHeaders() = default;

        ~CurlHeaders() {
            if (list_ != nullptr) {
                curl_slist_free_all(list_);
            }
        }

        CurlHeaders(const CurlHeaders &) = delete;
        CurlHeaders &operator=(const CurlHeaders &) = delete;
        CurlHeaders(CurlHeaders &&other) noexcept
        : list_(other.list_) {
            other.list_ = nullptr;
        }
        CurlHeaders &operator=(CurlHeaders &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            if (list_ != nullptr) {
                curl_slist_free_all(list_);
            }
            list_ = other.list_;
            other.list_ = nullptr;
            return *this;
        }

        void append(const std::string &header) {
            list_ = curl_slist_append(list_, header.c_str());
        }

        [[nodiscard]]
        struct curl_slist *get() const {
            return list_;
        }

    private:
        struct curl_slist *list_ = nullptr;
    };

    struct HeaderFallback {
        std::string key;
        std::string fallback;
    };

    inline void append_header(CurlHeaders &headers, std::string_view key, std::string_view value) {
        std::string header{key};
        header += ": ";
        header += value;
        headers.append(header);
    }

    [[nodiscard]]
    inline std::optional<std::string_view> find_header_value(const utils::transparent_string_unordered_map<std::string> &custom_headers, std::string_view key) {
        if (auto it = utils::transparent_find(custom_headers, key); it != custom_headers.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    [[nodiscard]]
    inline CurlHeaders compose_headers(const utils::transparent_string_unordered_map<std::string> &custom_headers, std::initializer_list<HeaderFallback> required_headers) {
        CurlHeaders headers;
        for (const auto &required : required_headers) {
            const auto value = find_header_value(custom_headers, required.key).value_or(required.fallback);
            append_header(headers, required.key, value);
        }

        for (const auto &[name, value] : custom_headers) {
            const auto is_required = std::ranges::any_of(required_headers, [&name](const HeaderFallback &required) {
                return required.key == name;
            });
            if (!is_required) {
                append_header(headers, name, value);
            }
        }
        return headers;
    }

    // Perform a blocking HTTP POST and return the response body
    // Throws on curl errors or non-200 HTTP status
    inline std::string http_post(const std::string &url, const std::string &body, const CurlHeaders &headers, long timeout = 120L) {
        CurlHandle curl;
        std::string response;

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);

        auto write_cb = +[](char *ptr, std::size_t size, std::size_t nmemb, std::string *data) -> std::size_t {
            data->append(ptr, size * nmemb);
            return size * nmemb;
        };
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl.get());
        long http_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(res));
        }
        if (http_code < 200 || http_code >= 300) {
            throw detail::make_http_status_error(http_code, response);
        }

        spdlog::debug("HTTP {} - response: {}", http_code, response);
        return response;
    }

    // Perform a blocking HTTP GET and return the response body
    // Throws on curl errors
    inline std::string http_get(const std::string &url, const CurlHeaders &headers, long timeout = 120L) {
        CurlHandle curl;
        std::string response;

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);

        auto write_cb = +[](char *ptr, std::size_t size, std::size_t nmemb, std::string *data) -> std::size_t {
            data->append(ptr, size * nmemb);
            return size * nmemb;
        };
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl.get());
        long http_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(res));
        }
        if (http_code < 200 || http_code >= 300) {
            throw detail::make_http_status_error(http_code, response);
        }

        spdlog::debug("HTTP {} - response: {}", http_code, response);
        return response;
    }

    // Perform a streaming HTTP POST, feeding data to a callback
    // Returns the HTTP status code
    template <typename StreamHandler>
    long http_post_stream(const std::string &url, const std::string &body, const CurlHeaders &headers, StreamHandler &handler, long timeout = 300L) {
        CurlHandle curl;

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);

        auto write_cb = +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *h = static_cast<StreamHandler *>(userdata);
            auto total = size * nmemb;
            h->feed(std::string_view(ptr, total));
            return total;
        };
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &handler);

        CURLcode res = curl_easy_perform(curl.get());
        long http_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(res));
        }

        return http_code;
    }

} // namespace orangutan::providers

namespace orangutan {

    using providers::CurlHandle;
    using providers::CurlHeaders;
    using providers::http_get;
    using providers::http_post;
    using providers::http_post_stream;

} // namespace orangutan
