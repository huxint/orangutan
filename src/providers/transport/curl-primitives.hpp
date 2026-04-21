#pragma once

#include <fmt/format.h>

#include <curl/curl.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace orangutan::providers::transport {

    class CurlHandle {
    public:
        CurlHandle()
        : handle_(curl_easy_init()) {
            if (handle_ == nullptr) {
                throw std::runtime_error("failed to initialize libcurl");
            }
        }

        ~CurlHandle() {
            if (handle_ != nullptr) {
                curl_easy_cleanup(handle_);
            }
        }

        CurlHandle(const CurlHandle &) = delete;
        CurlHandle &operator=(const CurlHandle &) = delete;
        CurlHandle(CurlHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
        CurlHandle &operator=(CurlHandle &&other) noexcept {
            if (this != &other) {
                if (handle_ != nullptr) {
                    curl_easy_cleanup(handle_);
                }
                handle_ = std::exchange(other.handle_, nullptr);
            }
            return *this;
        }

        [[nodiscard]]
        CURL *get() const noexcept {
            return handle_;
        }

    private:
        CURL *handle_ = nullptr;
    };

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
        : list_(std::exchange(other.list_, nullptr)) {}
        CurlHeaders &operator=(CurlHeaders &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            if (list_ != nullptr) {
                curl_slist_free_all(list_);
            }
            list_ = std::exchange(other.list_, nullptr);
            return *this;
        }

        void append(std::string_view header) {
            const auto owned = std::string(header);
            list_ = curl_slist_append(list_, owned.c_str());
        }

        void append(std::string_view key, std::string_view value) {
            const auto header = fmt::format("{}: {}", key, value);
            list_ = curl_slist_append(list_, header.c_str());
        }

        [[nodiscard]]
        curl_slist *get() const noexcept {
            return list_;
        }

    private:
        curl_slist *list_ = nullptr;
    };

} // namespace orangutan::providers::transport
