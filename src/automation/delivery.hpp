#pragma once

#include "automation/model.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::automation {

    /// Represents the rendered content for one automation delivery.
    struct DeliveryMessage {
        std::string title;
        std::string body;
    };

    using AutomationExecutor = std::function<ExecutionResult(const Automation &)>;
    using AutomationNotifier = std::function<std::optional<std::string>(std::string_view target, std::string_view title, std::string_view body)>;

    [[nodiscard]]
    DeliveryMessage make_delivery_message(const Automation &automation, const ExecutionResult &result);

} // namespace orangutan::automation
