#include "heartbeat/heartbeat-automation.hpp"

#include "heartbeat/heartbeat-ok.hpp"

#include <algorithm>

namespace orangutan::heartbeat {
    namespace {

        [[nodiscard]]
        bool has_tag(const automation::Automation &automation, std::string_view tag) {
            return std::ranges::find(automation.tags, tag) != automation.tags.end();
        }

        [[nodiscard]]
        std::string_view result_body(const automation::ExecutionResult &result) {
            if (!result.reply.empty()) {
                return result.reply;
            }

            return result.summary;
        }

    } // namespace

    bool is_heartbeat_automation(const automation::Automation &automation) {
        return has_tag(automation, HEARTBEAT_AUTOMATION_TAG);
    }

    bool is_managed_heartbeat_automation(const automation::Automation &automation) {
        return has_tag(automation, MANAGED_HEARTBEAT_AUTOMATION_TAG);
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

} // namespace orangutan::heartbeat
