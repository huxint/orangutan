#pragma once

#include "automation/model.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    struct DueAutomation {
        Automation automation;
        std::int64_t scheduled_for = 0;
    };

    [[nodiscard]]
    std::optional<std::int64_t> plan_next_due(const Automation &automation, TimePoint from);

    [[nodiscard]]
    bool is_automation_due(const Automation &automation, TimePoint now);

    [[nodiscard]]
    std::vector<DueAutomation> collect_due_automations(std::span<const Automation> automations, TimePoint now);

} // namespace orangutan::automation
