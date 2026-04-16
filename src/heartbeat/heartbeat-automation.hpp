#pragma once

#include "automation/category.hpp"
#include "automation/delivery.hpp"

#include <optional>
#include <string_view>

namespace orangutan::heartbeat {

    inline constexpr std::string_view HEARTBEAT_AUTOMATION_TAG = "heartbeat";
    inline constexpr std::string_view MANAGED_HEARTBEAT_AUTOMATION_TAG = "managed:heartbeat-config";

    [[nodiscard]]
    bool is_heartbeat_automation(const automation::Automation &automation);

    [[nodiscard]]
    bool is_managed_heartbeat_automation(const automation::Automation &automation);

    [[nodiscard]]
    std::optional<automation::DeliveryDisposition> heartbeat_delivery_disposition(const automation::Automation &automation, const automation::ExecutionResult &result,
                                                                                  int ack_max_chars);

    /// Build the automation category that identifies heartbeat-tagged automations
    /// and suppresses their delivery when the reply carries the `HEARTBEAT_OK`
    /// sentinel. Register via `AutomationService::register_category`.
    [[nodiscard]]
    automation::AutomationCategory make_heartbeat_category(int ack_max_chars);

} // namespace orangutan::heartbeat

namespace orangutan {

    using heartbeat::heartbeat_delivery_disposition;
    using heartbeat::is_heartbeat_automation;
    using heartbeat::is_managed_heartbeat_automation;
    using heartbeat::make_heartbeat_category;

} // namespace orangutan
