#include "heartbeat/heartbeat-automation.hpp"

#include "heartbeat/heartbeat-ok.hpp"

namespace orangutan::heartbeat {
    namespace {

        [[nodiscard]]
        std::string_view result_body(const automation::ExecutionResult &result) {
            if (!result.reply.empty()) {
                return result.reply;
            }

            return result.summary;
        }

    } // namespace

    bool is_heartbeat_automation(const automation::Automation &automation) {
        return automation::has_tag(automation, HEARTBEAT_AUTOMATION_TAG);
    }

    bool is_managed_heartbeat_automation(const automation::Automation &automation) {
        return automation::has_tag(automation, MANAGED_HEARTBEAT_AUTOMATION_TAG);
    }

    std::optional<automation::DeliveryDisposition> heartbeat_delivery_disposition(const automation::Automation &automation, const automation::ExecutionResult &result,
                                                                                  int ack_max_chars) {
        if (!is_heartbeat_automation(automation)) {
            return std::nullopt;
        }

        if (!detect_heartbeat_ok(result_body(result), ack_max_chars)) {
            return std::nullopt;
        }

        return automation::DeliveryDisposition{
            .suppress = true,
            .status = "heartbeat_ok",
        };
    }

    automation::AutomationCategory make_heartbeat_category(int ack_max_chars) {
        return automation::AutomationCategory{
            .tag = std::string(HEARTBEAT_AUTOMATION_TAG),
            .managed_tag = std::string(MANAGED_HEARTBEAT_AUTOMATION_TAG),
            .delivery_filter = [ack_max_chars](const automation::Automation &automation, const automation::ExecutionResult &result) {
                return heartbeat_delivery_disposition(automation, result, ack_max_chars);
            },
        };
    }

} // namespace orangutan::heartbeat
