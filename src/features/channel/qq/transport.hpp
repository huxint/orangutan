#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace orangutan::qq {

class Transport {
public:
    struct Callbacks {
        std::function<void()> on_open;
        std::function<void(std::string)> on_text;
        std::function<void(uint16_t, std::string)> on_close;
        std::function<void(std::string)> on_error;
    };

    explicit Transport(Callbacks callbacks);
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

} // namespace orangutan::qq
