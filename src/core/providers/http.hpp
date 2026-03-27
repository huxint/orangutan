#pragma once

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

namespace orangutan {

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
        CurlHeaders(CurlHeaders &&) = delete;
        CurlHeaders &operator=(CurlHeaders &&) = delete;

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

    // Perform a blocking HTTP POST and return the response body
    // Throws on curl errors or non-200 HTTP status
    inline std::string http_post(const std::string &url, const std::string &body, const CurlHeaders &headers, long timeout = 120L) {
        CurlHandle curl;
        std::string response;

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);

        auto write_cb = +[](char *ptr, size_t size, size_t nmemb, std::string *data) -> size_t {
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

        spdlog::debug("HTTP {} — Response: {}", http_code, response);
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

        auto write_cb = +[](char *ptr, size_t size, size_t nmemb, std::string *data) -> size_t {
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

        spdlog::debug("HTTP {} — Response: {}", http_code, response);
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

        auto write_cb = +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
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

} // namespace orangutan
