#include "automation/delivery.hpp"

namespace orangutan::automation {

    DeliveryMessage make_delivery_message(const Automation &automation, const ExecutionResult &result) {
        DeliveryMessage message;
        message.title = automation.name;
        message.body = result.reply.empty() ? result.summary : result.reply;
        return message;
    }

} // namespace orangutan::automation
