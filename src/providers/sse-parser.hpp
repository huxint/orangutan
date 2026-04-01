#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace orangutan {

    class SseParser {
    public:
        using EventCallback = std::function<void(const std::string &event, const std::string &data)>;

        explicit SseParser(EventCallback callback);
        void feed(std::string_view data);

    private:
        EventCallback callback_;
        std::string buffer_;
        std::string current_event_;
        std::string current_data_;

        void process_line(std::string_view line);
        void emit_event();
    };

} // namespace orangutan
