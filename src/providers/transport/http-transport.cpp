#include "providers/transport/http-transport.hpp"

#include <curl/curl.h>
#include <utility>

#include <spdlog/spdlog.h>

#include "providers/transport/sse-parser.hpp"

namespace orangutan::providers::transport {
    namespace {

        class CurlHandle {
        public:
            CurlHandle()
            : handle_(curl_easy_init()) {
                if (handle_ == nullptr) {
                    throw ProviderError(error_category::network, "failed to initialize libcurl");
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

            void append(std::string_view key, std::string_view value) {
                std::string header(key);
                header += ": ";
                header += value;
                list_ = curl_slist_append(list_, header.c_str());
            }

            [[nodiscard]]
            curl_slist *get() const noexcept {
                return list_;
            }

        private:
            curl_slist *list_ = nullptr;
        };

        [[nodiscard]]
        std::string truncate_body(std::string_view body, std::size_t max_chars = 512) {
            if (body.size() <= max_chars) {
                return std::string(body);
            }

            std::string truncated(body.substr(0, max_chars));
            truncated += "... (truncated)";
            return truncated;
        }

        [[nodiscard]]
        error_category category_for_status(long status_code) noexcept {
            if (status_code == 401 || status_code == 403) {
                return error_category::authentication;
            }
            if (status_code == 429) {
                return error_category::rate_limit;
            }
            if (status_code >= 500) {
                return error_category::upstream;
            }
            if (status_code >= 400) {
                return error_category::invalid_request;
            }
            return error_category::unknown;
        }

        [[nodiscard]]
        ProviderError make_http_error(long status_code, std::string_view body, const ModelTarget &target) {
            std::string message = "http request failed with status " + std::to_string(status_code);
            const auto snippet = truncate_body(body);
            if (!snippet.empty()) {
                message += ": " + snippet;
            }
            return ProviderError(category_for_status(status_code), std::move(message), target);
        }

        void append_bounded_preview(std::string &preview, std::string_view chunk, std::size_t max_chars = 1024) {
            if (preview.size() >= max_chars) {
                return;
            }

            const auto remaining = max_chars - preview.size();
            preview.append(chunk.substr(0, remaining));
        }

        void configure_request(CURL *handle, const HttpRequest &request, const CurlHeaders &headers) {
            curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle, CURLOPT_TIMEOUT, request.timeout_seconds);
        }

        [[nodiscard]]
        CurlHeaders make_headers(const header_map &headers) {
            CurlHeaders curl_headers;
            for (const auto &[key, value] : headers) {
                curl_headers.append(key, value);
            }
            return curl_headers;
        }

    } // namespace

    HttpResponse HttpTransport::post(const HttpRequest &request, const ModelTarget &target) const {
        CurlHandle handle;
        std::string response_body;
        const auto headers = make_headers(request.headers);

        configure_request(handle.get(), request, headers);

        const auto write_callback = +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *buffer = static_cast<std::string *>(userdata);
            buffer->append(ptr, size * nmemb);
            return size * nmemb;
        };
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response_body);

        const auto result_code = curl_easy_perform(handle.get());
        long status_code = 0;
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);

        if (result_code != CURLE_OK) {
            throw ProviderError(error_category::network, std::string("http request failed: ") + curl_easy_strerror(result_code), target);
        }
        if (status_code < 200 || status_code >= 300) {
            throw make_http_error(status_code, response_body, target);
        }

        spdlog::debug("http {} {} succeeded", request.url, status_code);
        return HttpResponse{.status_code = status_code, .body = std::move(response_body)};
    }

    void HttpTransport::stream_sse(const HttpRequest &request, const ModelTarget &target, const sse_event_callback &on_event) const {
        CurlHandle handle;
        std::string error_preview;
        const auto headers = make_headers(request.headers);
        SseParser parser([&](std::string_view event_name, std::string_view data) {
            if (on_event != nullptr) {
                on_event(event_name, data);
            }
        });

        configure_request(handle.get(), request, headers);
        struct StreamState {
            SseParser *parser = nullptr;
            std::string *error_preview = nullptr;
        };
        const auto write_callback = +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *state = static_cast<StreamState *>(userdata);
            const auto total = size * nmemb;
            const auto chunk = std::string_view(ptr, total);
            append_bounded_preview(*state->error_preview, chunk);
            state->parser->feed(chunk);
            return total;
        };
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
        auto state = StreamState{
            .parser = &parser,
            .error_preview = &error_preview,
        };
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);

        const auto result_code = curl_easy_perform(handle.get());
        long status_code = 0;
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);

        if (result_code != CURLE_OK) {
            throw ProviderError(error_category::network, std::string("http request failed: ") + curl_easy_strerror(result_code), target);
        }
        if (status_code < 200 || status_code >= 300) {
            throw make_http_error(status_code, error_preview, target);
        }

        spdlog::debug("http stream {} {} succeeded", request.url, status_code);
    }

} // namespace orangutan::providers::transport
