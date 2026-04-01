#pragma once

#include "features/automation/types.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace orangutan::builtin::detail {

    using ParseError = std::string;

    [[nodiscard]]
    std::expected<automation::DeliveryPolicy, ParseError> parse_delivery_overlay(const nlohmann::json &input, const automation::DeliveryPolicy &base);
    [[nodiscard]]
    std::expected<std::optional<std::vector<automation::ActiveHourWindow>>, ParseError> parse_active_hours_overlay(const nlohmann::json &input);

} // namespace orangutan::builtin::detail
