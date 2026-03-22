#include "features/channel/qq/transport.hpp"

#include "features/channel/qq/reconnect-backoff.hpp"

#include <stdexcept>

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>
#include <deque>
#include <future>
#include <atomic>
#include <string_view>
#include <thread>
#include <utility>
#endif

namespace orangutan::qq {

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

struct ParsedWebsocketUrl {
    std::string host;
    std::string host_header;
    std::string port;
    std::string target;
};

ParsedWebsocketUrl parse_websocket_url(const std::string &url) {
    constexpr std::string_view secure_prefix = "wss://";
    if (!url.starts_with(secure_prefix)) {
        throw std::runtime_error("QQ gateway URL must use wss://");
    }

    const auto authority_start = secure_prefix.size();
    const auto target_start = url.find('/', authority_start);
    const auto query_start = url.find('?', authority_start);
    const auto split_at = std::min(target_start == std::string::npos ? url.size() : target_start, query_start == std::string::npos ? url.size() : query_start);

    const auto authority = url.substr(authority_start, split_at - authority_start);
    if (authority.empty()) {
        throw std::runtime_error("QQ gateway URL is missing a host");
    }

    ParsedWebsocketUrl parsed;
    parsed.target = split_at == url.size() ? "/" : url.substr(split_at);

    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos) {
            throw std::runtime_error("QQ gateway URL contains an invalid IPv6 host");
        }

        parsed.host = authority.substr(1, end - 1);
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') {
                throw std::runtime_error("QQ gateway URL contains an invalid host/port pair");
            }
            parsed.port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            parsed.host = authority.substr(0, colon);
            parsed.port = authority.substr(colon + 1);
        } else {
            parsed.host = authority;
        }
    }

    if (parsed.host.empty()) {
        throw std::runtime_error("QQ gateway URL is missing a host");
    }
    if (parsed.port.empty()) {
        parsed.port = "443";
    }

    parsed.host_header = authority;
    return parsed;
}

std::string error_to_string(std::string_view context, const beast::error_code &ec) {
    return std::string(context) + ": " + ec.message();
}

} // namespace

class Transport::Impl : public std::enable_shared_from_this<Transport::Impl> {
public:
    explicit Impl(Callbacks callbacks)
    : callbacks_(std::move(callbacks)),
      work_guard_(asio::make_work_guard(io_context_)),
      ssl_context_(ssl::context::tls_client),
      resolver_(asio::make_strand(io_context_)),
      reconnect_timer_(io_context_) {
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_peer);
        io_thread_ = std::thread([this] {
            io_thread_id_ = std::this_thread::get_id();
            io_context_.run();
        });
    }

    ~Impl() = default;
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    void start(const std::string &url) {
        parsed_url_ = parse_websocket_url(url);
        asio::post(io_context_, [self = shared_from_this()] {
            self->stop_requested_ = false;
            self->reconnect_requested_ = false;
            self->reconnect_scheduled_ = false;
            self->reconnect_timer_.cancel();
            self->write_queue_.clear();
            self->write_in_progress_ = false;
            self->backoff_.reset();
            self->begin_connect();
        });
    }

    void send_text(std::string payload) {
        if (stop_requested_) {
            throw std::runtime_error("QQ WebSocket is stopped");
        }

        if (std::this_thread::get_id() == io_thread_id_) {
            enqueue_write(std::move(payload));
            return;
        }

        std::promise<void> ready;
        auto future = ready.get_future();
        asio::post(io_context_, [self = shared_from_this(), payload = std::move(payload), ready = std::move(ready)]() mutable {
            try {
                self->enqueue_write(std::move(payload));
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
            }
        });
        future.get();
    }

    void request_reconnect() {
        asio::post(io_context_, [self = shared_from_this()] {
            if (self->stop_requested_) {
                return;
            }

            self->reconnect_requested_ = true;
            self->reconnect_scheduled_ = false;
            self->open_ = false;
            self->write_queue_.clear();
            self->write_in_progress_ = false;

            if (!self->websocket_) {
                self->schedule_reconnect();
                return;
            }

            self->websocket_->next_layer().next_layer().cancel();
            self->websocket_->async_close(websocket::close_code::normal, [self](const beast::error_code &close_ec) {
                if (close_ec && close_ec != websocket::error::closed && close_ec != asio::error::operation_aborted) {
                    self->emit_error(error_to_string("QQ WebSocket close", close_ec));
                }
                self->websocket_.reset();
                self->schedule_reconnect();
            });
        });
    }

    void stop() {
        if (stopped_.exchange(true)) {
            if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) {
                io_thread_.join();
            }
            return;
        }

        stop_requested_ = true;
        asio::post(io_context_, [self = shared_from_this()] {
            self->reconnect_timer_.cancel();
            self->write_queue_.clear();
            self->write_in_progress_ = false;
            self->open_ = false;
            self->reconnect_scheduled_ = false;

            beast::error_code ec;
            self->resolver_.cancel();
            if (self->websocket_) {
                self->websocket_->next_layer().next_layer().cancel();
                [[maybe_unused]]
                const auto shutdown_result = self->websocket_->next_layer().next_layer().socket().shutdown(tcp::socket::shutdown_both, ec);
                [[maybe_unused]]
                const auto close_result = self->websocket_->next_layer().next_layer().socket().close(ec);
                self->websocket_.reset();
            }
        });

        work_guard_.reset();
        io_context_.stop();
        if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) {
            io_thread_.join();
        }
    }

private:
    using WebsocketStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    void begin_connect() {
        if (stop_requested_) {
            return;
        }

        reconnect_requested_ = false;
        reconnect_scheduled_ = false;
        open_ = false;
        read_buffer_.consume(read_buffer_.size());
        websocket_ = std::make_unique<WebsocketStream>(asio::make_strand(io_context_), ssl_context_);

        if (!SSL_set_tlsext_host_name(websocket_->next_layer().native_handle(), parsed_url_.host.c_str())) {
            const beast::error_code ec(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
            handle_failure("QQ WebSocket SNI", ec);
            return;
        }

        websocket_->next_layer().set_verify_mode(ssl::verify_peer);
        websocket_->next_layer().set_verify_callback(ssl::host_name_verification(parsed_url_.host));

        resolver_.async_resolve(parsed_url_.host, parsed_url_.port, [self = shared_from_this()](const beast::error_code &ec, const tcp::resolver::results_type &results) {
            self->on_resolve(ec, results);
        });
    }

    void on_resolve(const beast::error_code &ec, const tcp::resolver::results_type &results) {
        if (ec) {
            handle_failure("QQ WebSocket resolve", ec);
            return;
        }
        if (stop_requested_) {
            return;
        }

        beast::get_lowest_layer(*websocket_).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(*websocket_)
            .async_connect(results, [self = shared_from_this()](const beast::error_code &connect_ec, const tcp::resolver::results_type::endpoint_type &) {
                self->on_connect(connect_ec);
            });
    }

    void on_connect(const beast::error_code &ec) {
        if (ec) {
            handle_failure("QQ WebSocket connect", ec);
            return;
        }
        if (stop_requested_) {
            return;
        }

        beast::get_lowest_layer(*websocket_).expires_never();
        websocket_->next_layer().async_handshake(ssl::stream_base::client, [self = shared_from_this()](const beast::error_code &handshake_ec) {
            self->on_ssl_handshake(handshake_ec);
        });
    }

    void on_ssl_handshake(const beast::error_code &ec) {
        if (ec) {
            handle_failure("QQ WebSocket TLS handshake", ec);
            return;
        }
        if (stop_requested_) {
            return;
        }

        websocket_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        websocket_->async_handshake(parsed_url_.host_header, parsed_url_.target, [self = shared_from_this()](const beast::error_code &handshake_ec) {
            self->on_websocket_handshake(handshake_ec);
        });
    }

    void on_websocket_handshake(const beast::error_code &ec) {
        if (ec) {
            handle_failure("QQ WebSocket handshake", ec);
            return;
        }
        if (stop_requested_) {
            return;
        }

        open_ = true;
        backoff_.reset();
        emit_open();
        do_read();
    }

    void do_read() {
        if (stop_requested_ || !websocket_) {
            return;
        }

        websocket_->async_read(read_buffer_, [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
            self->on_read(ec);
        });
    }

    void on_read(const beast::error_code &ec) {
        if (ec == websocket::error::closed) {
            const auto reason = websocket_ ? websocket_->reason() : websocket::close_reason{};
            const auto code = static_cast<uint16_t>(reason.code);
            const char *message = reason.reason.c_str();
            websocket_.reset();
            open_ = false;
            write_queue_.clear();
            write_in_progress_ = false;

            if (stop_requested_) {
                return;
            }
            if (!reconnect_requested_) {
                emit_close(code, message);
            }
            schedule_reconnect();
            return;
        }

        if (ec) {
            handle_failure("QQ WebSocket read", ec);
            return;
        }
        if (stop_requested_) {
            return;
        }

        const auto text = beast::buffers_to_string(read_buffer_.cdata());
        read_buffer_.consume(read_buffer_.size());
        emit_text(text);
        do_read();
    }

    void enqueue_write(std::string payload) {
        if (stop_requested_) {
            throw std::runtime_error("QQ WebSocket is stopped");
        }
        if (!websocket_ || !open_) {
            throw std::runtime_error("QQ WebSocket is not connected");
        }

        write_queue_.push_back(std::move(payload));
        if (!write_in_progress_) {
            do_write();
        }
    }

    void do_write() {
        if (!websocket_ || write_queue_.empty()) {
            write_in_progress_ = false;
            return;
        }

        write_in_progress_ = true;
        websocket_->text(true);
        websocket_->async_write(asio::buffer(write_queue_.front()), [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
            self->on_write(ec);
        });
    }

    void on_write(const beast::error_code &ec) {
        if (ec) {
            handle_failure("QQ WebSocket write", ec);
            return;
        }

        write_queue_.pop_front();
        if (write_queue_.empty()) {
            write_in_progress_ = false;
            return;
        }
        do_write();
    }

    void handle_failure(std::string_view context, const beast::error_code &ec) {
        if (stop_requested_) {
            return;
        }
        if (reconnect_requested_) {
            return;
        }

        open_ = false;
        write_queue_.clear();
        write_in_progress_ = false;
        if (websocket_) {
            beast::error_code ignored;
            [[maybe_unused]]
            const auto close_result = websocket_->next_layer().next_layer().socket().close(ignored);
            websocket_.reset();
        }

        emit_error(error_to_string(context, ec));
        schedule_reconnect();
    }

    void schedule_reconnect() {
        if (stop_requested_) {
            return;
        }
        if (reconnect_scheduled_) {
            return;
        }

        const auto delay = backoff_.next_delay();
        reconnect_requested_ = false;
        reconnect_scheduled_ = true;
        reconnect_timer_.expires_after(delay);
        reconnect_timer_.async_wait([self = shared_from_this()](const beast::error_code &ec) {
            if (ec || self->stop_requested_) {
                return;
            }
            self->begin_connect();
        });
    }

    void emit_open() const {
        if (!callbacks_.on_open) {
            return;
        }
        try {
            callbacks_.on_open();
        } catch (const std::exception &e) {
            spdlog::error("QQ websocket open callback failed: {}", e.what());
        }
    }

    void emit_text(const std::string &text) const {
        if (!callbacks_.on_text) {
            return;
        }
        try {
            callbacks_.on_text(text);
        } catch (const std::exception &e) {
            spdlog::error("QQ websocket text callback failed: {}", e.what());
        }
    }

    void emit_close(uint16_t code, const std::string &reason) const {
        if (!callbacks_.on_close) {
            return;
        }
        try {
            callbacks_.on_close(code, reason);
        } catch (const std::exception &e) {
            spdlog::error("QQ websocket close callback failed: {}", e.what());
        }
    }

    void emit_error(const std::string &error) const {
        if (!callbacks_.on_error) {
            return;
        }
        try {
            callbacks_.on_error(error);
        } catch (const std::exception &e) {
            spdlog::error("QQ websocket error callback failed: {}", e.what());
        }
    }

    Callbacks callbacks_;
    asio::io_context io_context_{1};
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    ssl::context ssl_context_;
    tcp::resolver resolver_;
    asio::steady_timer reconnect_timer_;
    beast::flat_buffer read_buffer_;
    std::unique_ptr<WebsocketStream> websocket_;
    std::deque<std::string> write_queue_;
    ReconnectBackoff backoff_;
    ParsedWebsocketUrl parsed_url_;
    std::thread io_thread_;
    std::thread::id io_thread_id_;
    std::atomic<bool> stopped_{false};
    bool stop_requested_ = false;
    bool reconnect_requested_ = false;
    bool reconnect_scheduled_ = false;
    bool open_ = false;
    bool write_in_progress_ = false;
};
#endif

Transport::Transport(Callbacks callbacks)
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
: impl_(std::make_shared<Impl>(std::move(callbacks))){}
#else
: impl_(nullptr) {
    static_cast<void>(callbacks);
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
    if (impl_) {
        impl_->request_reconnect();
    }
#endif
}

void Transport::stop() {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
    if (impl_) {
        impl_->stop();
    }
#endif
}

} // namespace orangutan::qq
