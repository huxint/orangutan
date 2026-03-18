#include "core/providers/sse-parser.hpp"

#include <utility>

namespace orangutan {

SseParser::SseParser(EventCallback callback)
: callback_(std::move(callback)) {}

void SseParser::feed(const char *data, size_t len) {
    buffer_.append(data, len);

    size_t pos = 0;
    while (pos < buffer_.size()) {
        const auto newline = buffer_.find('\n', pos);
        if (newline == std::string::npos) {
            break;
        }

        auto line = std::string_view(buffer_).substr(pos, newline - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        process_line(line);
        pos = newline + 1;
    }

    if (pos > 0) {
        buffer_.erase(0, pos);
    }
}

void SseParser::process_line(std::string_view line) {
    if (line.empty()) {
        emit_event();
        return;
    }

    if (line.starts_with("event:")) {
        current_event_ = std::string(line.substr(6));
        if (!current_event_.empty() && current_event_.front() == ' ') {
            current_event_.erase(0, 1);
        }
        return;
    }

    if (line.starts_with("data:")) {
        auto data = line.substr(5);
        if (!data.empty() && data.front() == ' ') {
            data.remove_prefix(1);
        }
        if (!current_data_.empty()) {
            current_data_ += '\n';
        }
        current_data_ += std::string(data);
    }
}

void SseParser::emit_event() {
    if (current_data_.empty()) {
        current_event_.clear();
        return;
    }

    callback_(current_event_, current_data_);
    current_event_.clear();
    current_data_.clear();
}

} // namespace orangutan
