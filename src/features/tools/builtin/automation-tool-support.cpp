#include "features/tools/builtin/automation-tool-support.hpp"

namespace orangutan::builtin::detail {

std::expected<automation::DeliveryPolicy, ParseError> parse_delivery_overlay(const json &input, const automation::DeliveryPolicy &base) {
    auto delivery = base;

    if (const auto it = input.find("delivery_mode"); it != input.end()) {
        if (!it->is_string()) {
            return std::unexpected("invalid delivery configuration");
        }
        const auto mode = automation::delivery_mode_from_string(it->get<std::string>());
        if (!mode.has_value()) {
            return std::unexpected("invalid delivery configuration");
        }
        delivery.mode = *mode;
    }

    if (const auto it = input.find("targets"); it != input.end()) {
        if (!it->is_array()) {
            return std::unexpected("invalid delivery configuration");
        }

        delivery.targets.clear();
        delivery.targets.reserve(it->size());
        for (const auto &item : *it) {
            if (!item.is_string()) {
                return std::unexpected("invalid delivery configuration");
            }
            delivery.targets.push_back(item.get<std::string>());
        }
    }

    return delivery;
}

std::expected<std::optional<std::vector<automation::ActiveHourWindow>>, ParseError> parse_active_hours_overlay(const json &input) {
    const auto it = input.find("active_hours");
    if (it == input.end()) {
        return std::optional<std::vector<automation::ActiveHourWindow>>{};
    }
    if (!it->is_array()) {
        return std::unexpected("invalid active_hours configuration");
    }

    std::vector<automation::ActiveHourWindow> windows;
    windows.reserve(it->size());
    for (const auto &item : *it) {
        if (!item.is_object()) {
            return std::unexpected("invalid active_hours configuration");
        }
        windows.push_back({
            .start_minute = item.value("start_minute", 0),
            .end_minute = item.value("end_minute", 24 * 60),
        });
    }
    return windows;
}

} // namespace orangutan::builtin::detail
