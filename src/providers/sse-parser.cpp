#include "providers/sse-parser.hpp"

#include <utility>

namespace orangutan::providers {
    SseParser::SseParser(EventCallback callback)
    : callback_(std::move(callback)) {}

    void SseParser::feed(std::string_view data) {
        std::size_t pos = 0;
        while (pos < data.size()) {
            const auto newline = data.find('\n', pos);
            if (newline == std::string_view::npos) {
                buffer_.append(data.substr(pos));
                return;
            }

            auto line = data.substr(pos, newline - pos);

            if (buffer_.empty()) {
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                process_line(line);
            } else {
                buffer_.append(line);
                auto full_line = std::string_view{buffer_};
                if (full_line.ends_with('\r')) {
                    full_line.remove_suffix(1);
                }
                process_line(full_line);
                buffer_.clear();
            }

            pos = newline + 1;
        }
    }

    void SseParser::process_line(std::string_view line) {
        if (line.empty()) {
            emit_event();
            return;
        }

        if (line.starts_with("event:")) {
            auto value = line.substr(6);
            if (value.starts_with(' ')) {
                value.remove_prefix(1);
            }
            current_event_ = static_cast<std::string>(value);
            return;
        }

        if (line.starts_with("data:")) {
            auto value = line.substr(5);
            if (value.starts_with(' ')) {
                value.remove_prefix(1);
            }
            current_data_ += value;
            current_data_ += '\n';
        }
    }

    void SseParser::emit_event() {
        if (!current_data_.empty()) {
            current_data_.pop_back(); // remove trailing '\n'
            callback_(current_event_, current_data_);
            current_data_.clear();
        }
        current_event_.clear();
    }

} // namespace orangutan::providers
