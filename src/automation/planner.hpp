#pragma once

#include "automation/model.hpp"

#include <optional>

namespace orangutan::automation {

    [[nodiscard]]
    std::optional<std::int64_t> plan_next_due(const Automation &automation, TimePoint from);

} // namespace orangutan::automation
