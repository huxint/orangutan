#pragma once

#include "types/base.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace orangutan::channel::qq {

    class Transport {
    public:
        struct Callbacks {
            std::function<void()> on_open;
            std::function<void(std::string)> on_text;
            std::function<void(base::u16, std::string)> on_close;
            std::function<void(std::string)> on_error;
        };

        struct Event {
            enum class Kind {
                Text,
                Close,
                Error,
            };

            Kind kind;
            std::string payload;
            base::u16 close_code = 1000;

            [[nodiscard]]
            static Event text(std::string payload) {
                return Event{.kind = Kind::Text, .payload = std::move(payload)};
            }

            [[nodiscard]]
            static Event close(base::u16 close_code, std::string reason) {
                return Event{.kind = Kind::Close, .payload = std::move(reason), .close_code = close_code};
            }

            [[nodiscard]]
            static Event error(std::string message) {
                return Event{.kind = Kind::Error, .payload = std::move(message)};
            }
        };

        class Connection {
        public:
            virtual ~Connection() = default;

            virtual void send_text(std::string payload) = 0;
            [[nodiscard]]
            virtual std::optional<Event> wait_event(std::chrono::milliseconds timeout) = 0;
            virtual void close() = 0;
        };

        using ConnectionFactory = std::function<std::unique_ptr<Connection>(const std::string &url)>;

        explicit Transport(Callbacks callbacks);
        Transport(Callbacks callbacks, ConnectionFactory connection_factory);
        ~Transport();

        Transport(const Transport &) = delete;
        Transport &operator=(const Transport &) = delete;
        Transport(Transport &&) = delete;
        Transport &operator=(Transport &&) = delete;

        void start(const std::string &url);
        void send_text(std::string payload);
        void request_reconnect();
        void stop();

    private:
        class Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    namespace qq = channel::qq;

} // namespace orangutan
