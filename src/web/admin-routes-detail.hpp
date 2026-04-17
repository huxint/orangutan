#pragma once

#include "web/errors.hpp"

#include <expected>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::web::admin_detail {

    /// Legacy shim — routes through the unified error envelope. Kept because admin helpers
    /// already accept `std::string_view` and we don't want to change call sites.
    inline void set_json_error(httplib::Response &res, int status, std::string_view message) {
        // Map status codes to short machine-readable codes so the frontend can branch on them
        // without parsing prose. Additional codes are added organically as needed.
        std::string_view code = "error";
        switch (status) {
            case 400: code = "bad_request"; break;
            case 401: code = "unauthorized"; break;
            case 403: code = "forbidden"; break;
            case 404: code = "not_found"; break;
            case 409: code = "conflict"; break;
            case 503: code = "service_unavailable"; break;
            default: break;
        }
        send_error(res, status, code, message);
    }

    [[nodiscard]]
    inline std::expected<nlohmann::json, std::string> parse_request_json(const httplib::Request &req) {
        try {
            return nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            return std::unexpected("invalid JSON");
        }
    }

    [[nodiscard]]
    inline std::expected<std::optional<bool>, std::string> parse_optional_bool_param(const httplib::Request &req, std::string_view key) {
        if (!req.has_param(std::string(key))) {
            return std::optional<bool>{};
        }

        const auto value = req.get_param_value(std::string(key));
        if (value == "true" || value == "1") {
            return std::optional<bool>{true};
        }
        if (value == "false" || value == "0") {
            return std::optional<bool>{false};
        }
        return std::unexpected(std::string(key) + " must be true or false");
    }

    [[nodiscard]]
    inline std::string resolve_optional_agent_key(const httplib::Request &req, const nlohmann::json *body = nullptr) {
        if (body != nullptr) {
            if (const auto it = body->find("agent_key"); it != body->end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
        if (req.has_param("agent_key")) {
            return req.get_param_value("agent_key");
        }
        return {};
    }

    [[nodiscard]]
    inline std::string resolve_agent_key_or_default(const httplib::Request &req, const nlohmann::json *body = nullptr) {
        const auto agent_key = resolve_optional_agent_key(req, body);
        return agent_key.empty() ? std::string{"default"} : agent_key;
    }

} // namespace orangutan::web::admin_detail
