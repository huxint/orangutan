#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "automation/model.hpp"

namespace orangutan::automation {

    /// Serializes a unified trigger to the canonical external wire format.
    [[nodiscard]]
    nlohmann::json trigger_to_json(const TriggerDefinition &trigger);

    /// Parses a unified trigger from the canonical external wire format.
    [[nodiscard]]
    std::expected<TriggerDefinition, std::string> trigger_from_json(const nlohmann::json &value);

    /// Parses a canonical duration string such as 30s, 15m, or 2h.
    [[nodiscard]]
    std::expected<std::chrono::seconds, std::string> parse_duration_string(std::string_view value);

} // namespace orangutan::automation
