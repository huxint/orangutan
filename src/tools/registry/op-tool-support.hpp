#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "tool-dispatch.hpp"

namespace orangutan::tools {

    [[nodiscard]]
    inline nlohmann::json routed_input_with_default_op(const nlohmann::json &input, std::string_view default_op) {
        auto routed = input;
        routed["op"] = input.value("op", std::string{default_op});
        return routed;
    }

    [[nodiscard]]
    inline std::optional<std::string> require_id_or_name(const nlohmann::json &request) {
        const auto id_or_name = request.value("id", request.value("name", ""));
        if (id_or_name.empty()) {
            return std::string{"Error: id or name is required."};
        }
        return std::nullopt;
    }

    [[nodiscard]]
    inline std::optional<std::string> require_id(const nlohmann::json &request) {
        const auto id = request.value("id", "");
        if (id.empty()) {
            return std::string{"Error: id is required."};
        }
        return std::nullopt;
    }

    [[nodiscard]]
    inline std::string dispatch_message(const ToolDispatch &dispatch, const nlohmann::json &input) {
        return dispatch.run(input).message;
    }

} // namespace orangutan::tools
