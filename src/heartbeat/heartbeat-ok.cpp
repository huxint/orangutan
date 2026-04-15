#include "heartbeat/heartbeat-ok.hpp"
#include "utils/string.hpp"

#include <string_view>

namespace orangutan::heartbeat {
    namespace {

        constexpr std::string_view HEARTBEAT_OK_TOKEN = "HEARTBEAT_OK";

    } // namespace

    bool detect_heartbeat_ok(std::string_view response, int ack_max_chars, std::string *out_stripped) {
        const auto trimmed = utils::trim_copy(response);
        if (trimmed.empty()) {
            return false;
        }

        std::string_view remainder;
        if (trimmed.starts_with(HEARTBEAT_OK_TOKEN)) {
            remainder = utils::trim_copy(trimmed.substr(HEARTBEAT_OK_TOKEN.size()));
        } else if (trimmed.ends_with(HEARTBEAT_OK_TOKEN)) {
            remainder = utils::trim_copy(trimmed.substr(0, trimmed.size() - HEARTBEAT_OK_TOKEN.size()));
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

    bool should_suppress_heartbeat_reply(std::string_view jid, std::string_view response, int ack_max_chars) {
        return jid.starts_with("heartbeat:") && detect_heartbeat_ok(response, ack_max_chars);
    }

} // namespace orangutan::heartbeat
