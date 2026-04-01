#include "heartbeat/heartbeat-ok.hpp"

#include <string_view>

namespace orangutan {
    namespace {

        constexpr std::string_view heartbeat_ok_token = "HEARTBEAT_OK";

        std::string_view trim(std::string_view sv) {
            auto start = sv.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) {
                return {};
            }
            auto end = sv.find_last_not_of(" \t\r\n");
            return sv.substr(start, end - start + 1);
        }

    } // namespace

    bool detect_heartbeat_ok(const std::string &response, int ack_max_chars, std::string *out_stripped) {
        auto trimmed = trim(response);
        if (trimmed.empty()) {
            return false;
        }

        std::string_view remainder;
        if (trimmed.starts_with(heartbeat_ok_token)) {
            remainder = trim(trimmed.substr(heartbeat_ok_token.size()));
        } else if (trimmed.ends_with(heartbeat_ok_token)) {
            remainder = trim(trimmed.substr(0, trimmed.size() - heartbeat_ok_token.size()));
        } else {
            return false;
        }

        if (ack_max_chars >= 0 && remainder.size() <= static_cast<std::size_t>(ack_max_chars)) {
            if (out_stripped != nullptr) {
                *out_stripped = std::string(remainder);
            }
            return true;
        }

        if (out_stripped != nullptr) {
            *out_stripped = std::string(remainder);
        }
        return false;
    }

    bool should_suppress_heartbeat_reply(std::string_view jid, const std::string &response, int ack_max_chars) {
        return jid.starts_with("heartbeat:") && detect_heartbeat_ok(response, ack_max_chars);
    }

} // namespace orangutan
