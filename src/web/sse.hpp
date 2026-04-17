#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace orangutan::web {

    /// Write a single Server-Sent Event frame. Keeps the format in one place so every
    /// handler (chat, event bus, observatory) emits the exact same wire bytes.
    inline bool write_sse(httplib::DataSink &sink, std::string_view event_name, const nlohmann::json &payload, std::string_view id = {}) {
        std::string frame;
        frame.reserve(64 + payload.dump().size());
        if (!id.empty()) {
            frame.append("id: ");
            frame.append(id);
            frame.push_back('\n');
        }
        frame.append("event: ");
        frame.append(event_name);
        frame.push_back('\n');
        frame.append("data: ");
        frame.append(payload.dump());
        frame.append("\n\n");
        return sink.write(frame.data(), frame.size());
    }

    /// Write a comment keep-alive line so intermediaries don't drop idle connections.
    inline bool write_sse_keepalive(httplib::DataSink &sink) {
        constexpr std::string_view PING = ": ping\n\n";
        return sink.write(PING.data(), PING.size());
    }

    /// Set the standard SSE response headers in one place.
    inline void prepare_sse_response(httplib::Response &res) {
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("X-Accel-Buffering", "no");
    }

} // namespace orangutan::web
