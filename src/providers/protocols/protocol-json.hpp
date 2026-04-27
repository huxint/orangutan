#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace orangutan::providers::protocols {

    [[nodiscard]]
    nlohmann::json parse_protocol_json_object(std::string_view payload, std::string_view protocol_label, std::string_view context);

} // namespace orangutan::providers::protocols
