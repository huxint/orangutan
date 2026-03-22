#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace orangutan {

class QqWebsocketClient {
public:
    struct Callbacks {
        std::function<void()> on_open;
        std::function<void(std::string)> on_text;
        std::function<void(uint16_t, std::string)> on_close;
        std::function<void(std::string)> on_error;
    };

    explicit QqWebsocketClient(Callbacks callbacks);
    ~QqWebsocketClient();

    QqWebsocketClient(const QqWebsocketClient &) = delete;
    QqWebsocketClient &operator=(const QqWebsocketClient &) = delete;
    QqWebsocketClient(QqWebsocketClient &&) = delete;
    QqWebsocketClient &operator=(QqWebsocketClient &&) = delete;

    void start(const std::string &url);
    void send_text(std::string payload);
    void request_reconnect();
    void stop();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace orangutan
