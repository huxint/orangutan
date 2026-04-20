#include "channel/qq/qq-transport.hpp"

#include "channel/qq/reconnect-backoff.hpp"
#include "types/base.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#endif

namespace orangutan::channel::qq {

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
    namespace {

        using Clock = std::chrono::steady_clock;

        void ensure_curl_ready() {
            static std::once_flag once;
            std::call_once(once, [] {
                const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
                if (code != CURLE_OK) {
                    throw std::runtime_error(std::string{"Failed to initialize libcurl: "} + curl_easy_strerror(code));
                }
            });
        }

        [[nodiscard]]
        std::string curl_message(std::string_view context, CURLcode code, std::string_view detail = {}) {
            std::string message(context);
            message += ": ";
            if (!detail.empty()) {
                message += detail;
            } else {
                message += curl_easy_strerror(code);
            }
            return message;
        }

        [[nodiscard]]
        std::string trim_curl_error(const char *buffer) {
            if (buffer == nullptr || buffer[0] == '\0') {
                return {};
            }
            std::string message(buffer);
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '\0')) {
                message.pop_back();
            }
            return message;
        }

        [[nodiscard]]
        Transport::Event parse_close_event(std::string payload) {
            base::u16 code = 1000;
            std::string reason;

            if (payload.size() >= sizeof(base::u16)) {
                base::u16 network_code = 0;
                std::memcpy(&network_code, payload.data(), sizeof(network_code));
                code = ntohs(network_code);
                reason = payload.substr(sizeof(base::u16));
            } else {
                reason = std::move(payload);
            }

            return Transport::Event::close(code, std::move(reason));
        }

        class CurlConnection final : public Transport::Connection {
        public:
            explicit CurlConnection(const std::string &url)
            : handle_([] {
                  ensure_curl_ready();
                  return curl_easy_init();
              }()) {
                if (handle_ == nullptr) {
                    throw std::runtime_error("Failed to initialize libcurl websocket handle");
                }

                error_buffer_[0] = '\0';
                curl_easy_setopt(handle_, CURLOPT_ERRORBUFFER, error_buffer_.data());
                curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
                curl_easy_setopt(handle_, CURLOPT_CONNECT_ONLY, 2L);
                curl_easy_setopt(handle_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
                curl_easy_setopt(handle_, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 30L);
                curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, 2L);

                const auto code = curl_easy_perform(handle_);
                if (code != CURLE_OK) {
                    const auto detail = trim_curl_error(error_buffer_.data());
                    curl_easy_cleanup(handle_);
                    handle_ = nullptr;
                    throw std::runtime_error(curl_message("QQ WebSocket connect", code, detail));
                }

                const auto socket_result = curl_easy_getinfo(handle_, CURLINFO_ACTIVESOCKET, &socket_);
                if (socket_result != CURLE_OK || socket_ == CURL_SOCKET_BAD) {
                    curl_easy_cleanup(handle_);
                    handle_ = nullptr;
                    throw std::runtime_error(curl_message("QQ WebSocket connect", socket_result, "connection socket unavailable"));
                }
            }

            ~CurlConnection() override {
                cleanup();
            }

            CurlConnection(const CurlConnection &) = delete;
            CurlConnection &operator=(const CurlConnection &) = delete;
            CurlConnection(CurlConnection &&) = delete;
            CurlConnection &operator=(CurlConnection &&) = delete;

            void send_text(std::string payload) override {
                ensure_open();

                std::size_t offset = 0;
                while (offset < payload.size()) {
                    std::size_t sent = 0;
                    const auto code = curl_ws_send(handle_, payload.data() + offset, payload.size() - offset, &sent, 0, CURLWS_TEXT);
                    offset += sent;
                    if (code == CURLE_OK) {
                        continue;
                    }
                    if (code == CURLE_AGAIN) {
                        if (!wait_socket(POLLOUT, std::chrono::seconds(30))) {
                            throw std::runtime_error("QQ WebSocket write timed out waiting for socket readiness");
                        }
                        continue;
                    }

                    throw std::runtime_error(curl_message("QQ WebSocket write", code, trim_curl_error(error_buffer_.data())));
                }
            }

            [[nodiscard]]
            std::optional<Transport::Event> wait_event(std::chrono::milliseconds timeout) override {
                ensure_open();

                if (!wait_socket(POLLIN, timeout)) {
                    return std::nullopt;
                }

                std::string payload;
                unsigned int flags = 0;

                while (true) {
                    std::array<char, 4096> buffer{};
                    std::size_t received = 0;
                    const curl_ws_frame *frame = nullptr;
                    const auto code = curl_ws_recv(handle_, buffer.data(), buffer.size(), &received, &frame);

                    if (code == CURLE_AGAIN) {
                        if (!wait_socket(POLLIN, timeout)) {
                            return std::nullopt;
                        }
                        continue;
                    }
                    if (code == CURLE_GOT_NOTHING) {
                        return Transport::Event::close(1000, {});
                    }
                    if (code != CURLE_OK) {
                        throw std::runtime_error(curl_message("QQ WebSocket read", code, trim_curl_error(error_buffer_.data())));
                    }
                    if (frame == nullptr) {
                        continue;
                    }

                    flags = frame->flags;
                    if (received > 0) {
                        payload.append(buffer.data(), received);
                    }
                    if (frame->bytesleft > 0) {
                        continue;
                    }

                    if ((flags & CURLWS_TEXT) != 0U) {
                        return Transport::Event::text(std::move(payload));
                    }
                    if ((flags & CURLWS_CLOSE) != 0U) {
                        return parse_close_event(std::move(payload));
                    }
                    if ((flags & CURLWS_BINARY) != 0U) {
                        return Transport::Event::error("QQ WebSocket received unexpected binary frame");
                    }

                    payload.clear();
                    flags = 0;
                }
            }

            void close() override {
                if (closed_) {
                    return;
                }

                const base::u16 code = htons(static_cast<base::u16>(1000));
                std::size_t sent = 0;
                const auto result = curl_ws_send(handle_, &code, sizeof(code), &sent, 0, CURLWS_CLOSE);
                if (result != CURLE_OK && result != CURLE_GOT_NOTHING) {
                    spdlog::debug("QQ WebSocket close frame failed: {}", curl_easy_strerror(result));
                }
                cleanup();
            }

        private:
            void ensure_open() const {
                if (closed_ || handle_ == nullptr) {
                    throw std::runtime_error("QQ WebSocket is closed");
                }
            }

            [[nodiscard]]
            bool wait_socket(short events, std::chrono::milliseconds timeout) const {
                if (socket_ == CURL_SOCKET_BAD) {
                    throw std::runtime_error("QQ WebSocket socket is invalid");
                }

                pollfd descriptor{.fd = socket_, .events = events, .revents = 0};
                const auto timeout_ms =
                    timeout.count() > static_cast<base::i64>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(timeout.count());
                const auto rc = ::poll(&descriptor, 1, timeout_ms);
                if (rc == 0) {
                    return false;
                }
                if (rc < 0) {
                    if (errno == EINTR) {
                        return false;
                    }
                    throw std::runtime_error(std::string{"QQ WebSocket poll failed: "} + std::strerror(errno));
                }
                return true;
            }

            void cleanup() {
                if (closed_) {
                    return;
                }

                closed_ = true;
                socket_ = CURL_SOCKET_BAD;
                if (handle_ != nullptr) {
                    curl_easy_cleanup(handle_);
                    handle_ = nullptr;
                }
            }

            CURL *handle_ = nullptr;
            curl_socket_t socket_ = CURL_SOCKET_BAD;
            std::array<char, CURL_ERROR_SIZE> error_buffer_{};
            bool closed_ = false;
        };

        [[nodiscard]]
        Transport::ConnectionFactory default_connection_factory() {
            return [](const std::string &url) {
                return std::make_unique<CurlConnection>(url);
            };
        }

    } // namespace

    class Transport::Impl : public std::enable_shared_from_this<Transport::Impl> {
    public:
        Impl(Callbacks callbacks, ConnectionFactory connection_factory)
        : callbacks_(std::move(callbacks)),
          connection_factory_(connection_factory ? std::move(connection_factory) : default_connection_factory()),
          worker_thread_([this] {
              run();
          }) {}

        ~Impl() = default;
        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;

        void start(const std::string &url) {
            {
                std::scoped_lock lock(mutex_);
                if (stopped_) {
                    throw std::runtime_error("QQ WebSocket is stopped");
                }
                url_ = url;
                write_queue_.clear();
                backoff_.reset();
                reconnect_requested_ = false;
                reconnect_scheduled_ = true;
                reconnect_at_ = Clock::now();
                open_ = false;
                connection_reset_requested_ = connection_ != nullptr;
            }
            cv_.notify_all();
        }

        void send_text(std::string payload) {
            {
                std::scoped_lock lock(mutex_);
                if (stopped_ || stop_requested_) {
                    throw std::runtime_error("QQ WebSocket is stopped");
                }
                if (!open_ || !connection_) {
                    throw std::runtime_error("QQ WebSocket is not connected");
                }
                write_queue_.push_back(std::move(payload));
            }
            cv_.notify_all();
        }

        void request_reconnect() {
            {
                std::scoped_lock lock(mutex_);
                if (stopped_ || stop_requested_) {
                    return;
                }
                reconnect_requested_ = true;
                reconnect_scheduled_ = true;
                reconnect_at_ = Clock::now() + backoff_.next_delay();
                open_ = false;
                write_queue_.clear();
                connection_reset_requested_ = connection_ != nullptr;
            }
            cv_.notify_all();
        }

        void stop() {
            {
                std::scoped_lock lock(mutex_);
                if (stopped_) {
                    finalize_join();
                    return;
                }
                stopped_ = true;
                stop_requested_ = true;
                open_ = false;
                write_queue_.clear();
                connection_reset_requested_ = true;
            }
            cv_.notify_all();
            finalize_join();
        }

    private:
        void run() {
            while (true) {
                std::unique_lock lock(mutex_);
                if (stop_requested_) {
                    break;
                }

                if (connection_reset_requested_) {
                    auto connection = std::move(connection_);
                    connection_reset_requested_ = false;
                    lock.unlock();
                    if (connection != nullptr) {
                        connection->close();
                    }
                    continue;
                }

                if (should_connect_locked()) {
                    const auto url = url_;
                    lock.unlock();

                    try {
                        auto connection = connection_factory_(url);
                        lock.lock();
                        if (stop_requested_ || connection_reset_requested_ || url != url_) {
                            if (!stop_requested_ && url != url_) {
                                reconnect_scheduled_ = true;
                                reconnect_at_ = Clock::now();
                            }
                            lock.unlock();
                            connection->close();
                            continue;
                        }

                        connection_ = std::move(connection);
                        open_ = true;
                        reconnect_requested_ = false;
                        reconnect_scheduled_ = false;
                        backoff_.reset();
                        lock.unlock();
                        emit_open();
                    } catch (const std::exception &e) {
                        lock.lock();
                        if (stop_requested_) {
                            break;
                        }
                        open_ = false;
                        if (!reconnect_scheduled_ || Clock::now() >= reconnect_at_) {
                            schedule_reconnect_locked();
                        }
                        lock.unlock();
                        emit_error(e.what());
                    }
                    continue;
                }

                if (connection_ && !write_queue_.empty()) {
                    auto payload = std::move(write_queue_.front());
                    write_queue_.pop_front();
                    auto *connection = connection_.get();
                    lock.unlock();
                    try {
                        connection->send_text(std::move(payload));
                    } catch (const std::exception &e) {
                        handle_connection_error(e.what());
                    }
                    continue;
                }

                if (!connection_) {
                    if (reconnect_scheduled_) {
                        cv_.wait_until(lock, reconnect_at_, [this] {
                            return stop_requested_ || connection_reset_requested_ || should_connect_locked();
                        });
                    } else {
                        cv_.wait(lock, [this] {
                            return stop_requested_ || connection_reset_requested_ || should_connect_locked();
                        });
                    }
                    continue;
                }

                auto *connection = connection_.get();
                lock.unlock();

                try {
                    auto event = connection->wait_event(std::chrono::milliseconds(100));
                    if (event.has_value()) {
                        handle_event(*event);
                    }
                } catch (const std::exception &e) {
                    handle_connection_error(e.what());
                }
            }

            std::unique_ptr<Connection> connection;
            {
                std::scoped_lock lock(mutex_);
                connection = std::move(connection_);
                write_queue_.clear();
                open_ = false;
            }
            if (connection) {
                connection->close();
            }
        }

        [[nodiscard]]
        bool should_connect_locked() const {
            return !url_.empty() && !connection_ && reconnect_scheduled_ && Clock::now() >= reconnect_at_;
        }

        void handle_event(const Event &event) {
            switch (event.kind) {
                case Event::kind::text:
                    emit_text(event.payload);
                    return;
                case Event::kind::close: {
                    bool emit_close_callback = false;
                    {
                        std::scoped_lock lock(mutex_);
                        connection_.reset();
                        open_ = false;
                        write_queue_.clear();
                        if (stop_requested_) {
                            return;
                        }
                        emit_close_callback = !reconnect_requested_;
                        schedule_reconnect_locked();
                    }
                    if (emit_close_callback) {
                        emit_close(event.close_code, event.payload);
                    }
                    return;
                }
                case Event::kind::error:
                    handle_connection_error(event.payload);
                    return;
            }
        }

        void handle_connection_error(const std::string &message) {
            std::unique_ptr<Connection> connection;
            {
                std::scoped_lock lock(mutex_);
                if (stop_requested_) {
                    return;
                }
                connection = std::move(connection_);
                open_ = false;
                write_queue_.clear();
                schedule_reconnect_locked();
            }
            if (connection) {
                connection->close();
            }
            emit_error(message);
        }

        void schedule_reconnect_locked() {
            reconnect_requested_ = false;
            reconnect_scheduled_ = true;
            reconnect_at_ = Clock::now() + backoff_.next_delay();
        }

        void finalize_join() {
            if (worker_thread_.joinable() && worker_thread_.get_id() != std::this_thread::get_id()) {
                worker_thread_.join();
            }
        }

        void emit_open() const {
            if (callbacks_.on_open == nullptr) {
                return;
            }
            try {
                callbacks_.on_open();
            } catch (const std::exception &e) {
                spdlog::error("QQ websocket open callback failed: {}", e.what());
            }
        }

        void emit_text(const std::string &text) const {
            if (callbacks_.on_text == nullptr) {
                return;
            }
            try {
                callbacks_.on_text(text);
            } catch (const std::exception &e) {
                spdlog::error("QQ websocket text callback failed: {}", e.what());
            }
        }

        void emit_close(base::u16 code, const std::string &reason) const {
            if (callbacks_.on_close == nullptr) {
                return;
            }
            try {
                callbacks_.on_close(code, reason);
            } catch (const std::exception &e) {
                spdlog::error("QQ websocket close callback failed: {}", e.what());
            }
        }

        void emit_error(const std::string &error) const {
            if (callbacks_.on_error == nullptr) {
                return;
            }
            try {
                callbacks_.on_error(error);
            } catch (const std::exception &e) {
                spdlog::error("QQ websocket error callback failed: {}", e.what());
            }
        }

        Callbacks callbacks_;
        ConnectionFactory connection_factory_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::string> write_queue_;
        ReconnectBackoff backoff_;
        std::unique_ptr<Connection> connection_;
        std::thread worker_thread_;
        std::string url_;
        Clock::time_point reconnect_at_ = Clock::time_point::min();
        bool stopped_ = false;
        bool stop_requested_ = false;
        bool reconnect_requested_ = false;
        bool reconnect_scheduled_ = false;
        bool connection_reset_requested_ = false;
        bool open_ = false;
    };
#endif

    Transport::Transport(Callbacks callbacks)
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
    : impl_(std::make_shared<Impl>(std::move(callbacks), ConnectionFactory{})){}
#else
    : impl_(nullptr) {
        static_cast<void>(callbacks);
    }
#endif

      Transport::Transport(Callbacks callbacks, ConnectionFactory connection_factory)
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
    : impl_(std::make_shared<Impl>(std::move(callbacks), std::move(connection_factory))){}
#else
    : impl_(nullptr) {
        static_cast<void>(callbacks);
        static_cast<void>(connection_factory);
    }
#endif

      Transport::~Transport() {
        stop();
    }

    void Transport::start(const std::string &url) {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        impl_->start(url);
#else
        static_cast<void>(url);
        throw std::runtime_error("QQ channel support was not compiled in");
#endif
    }

    void Transport::send_text(std::string payload) {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        impl_->send_text(std::move(payload));
#else
        static_cast<void>(payload);
        throw std::runtime_error("QQ channel support was not compiled in");
#endif
    }

    void Transport::request_reconnect() {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        if (impl_ != nullptr) {
            impl_->request_reconnect();
        }
#endif
    }

    void Transport::stop() {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        if (impl_ != nullptr) {
            impl_->stop();
        }
#endif
    }

} // namespace orangutan::channel::qq
