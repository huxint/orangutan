#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace orangutan::web {

    /// Unified API error envelope: `{"error": {"code": "...", "message": "...", "details": ...}}`.
    /// Every v1 handler uses this shape so the frontend has a single error contract.
    inline void send_error(httplib::Response &res, int status, std::string_view code, std::string_view message,
                           std::optional<nlohmann::json> details = std::nullopt) {
        nlohmann::json envelope = {
            {"error", {
                         {"code", code},
                         {"message", message},
                     }},
        };
        if (details.has_value()) {
            envelope["error"]["details"] = std::move(*details);
        }
        res.status = status;
        res.set_content(envelope.dump(), "application/json");
    }

    inline void send_json(httplib::Response &res, const nlohmann::json &body, int status = 200) {
        res.status = status;
        res.set_content(body.dump(), "application/json");
    }

    /// Parse request JSON body; returns std::nullopt and writes 400 on failure.
    [[nodiscard]]
    inline std::optional<nlohmann::json> parse_body(const httplib::Request &req, httplib::Response &res) {
        try {
            return nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            send_error(res, 400, "invalid_json", "request body is not valid JSON");
            return std::nullopt;
        }
    }

} // namespace orangutan::web
