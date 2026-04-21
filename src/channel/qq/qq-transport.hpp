#pragma once

#include "types/base.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace orangutan::utils {
    class TaskPool;
}

namespace orangutan::channel::qq {

    class Transport {
    public:
        struct Callbacks {
            std::function<void()> on_open;
            std::function<void(std::string)> on_text;
            std::function<void(std::uint16_t, std::string)> on_close;
            std::function<void(std::string)> on_error;
        };

        struct Event {
            enum class kind : std::uint8_t {
                text,
                close,
                error,
            };

            kind kind;
            std::string payload;
            std::uint16_t close_code = 1000;

            [[nodiscard]]
            static Event text(std::string payload) {
                return Event{.kind = kind::text, .payload = std::move(payload)};
            }

            [[nodiscard]]
            static Event close(std::uint16_t close_code, std::string reason) {
                return Event{.kind = kind::close, .payload = std::move(reason), .close_code = close_code};
            }

            [[nodiscard]]
            static Event error(std::string message) {
                return Event{.kind = kind::error, .payload = std::move(message)};
            }
        };

        class Connection {
        public:
            Connection(const Connection &) = delete;
            Connection &operator=(const Connection &) = delete;
            Connection(Connection &&) = delete;
            Connection &operator=(Connection &&) = delete;
            virtual ~Connection() = default;

            virtual void send_text(std::string payload) = 0;
            [[nodiscard]]
            virtual std::optional<Event> wait_event(std::chrono::milliseconds timeout) = 0;
            virtual void close() = 0;

        protected:
            Connection() = default;
        };

        using ConnectionFactory = std::function<std::unique_ptr<Connection>(const std::string &url)>;

        explicit Transport(Callbacks callbacks, utils::TaskPool &task_pool);
        Transport(Callbacks callbacks, ConnectionFactory connection_factory, utils::TaskPool &task_pool);
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
