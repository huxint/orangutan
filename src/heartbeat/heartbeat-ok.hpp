#pragma once

#include <string>
#include <string_view>

namespace orangutan {

    /// Detect the HEARTBEAT_OK sentinel in a heartbeat response.
    /// Returns true if the message should be suppressed (dropped silently).
    /// If true and out_stripped is non-null, writes the stripped remaining text.
    bool detect_heartbeat_ok(const std::string &response, int ack_max_chars, std::string *out_stripped = nullptr);

    /// Returns true when a heartbeat reply should be dropped silently.
    [[nodiscard]]
    bool should_suppress_heartbeat_reply(std::string_view jid, const std::string &response, int ack_max_chars);

} // namespace orangutan
