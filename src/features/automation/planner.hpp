#pragma once

#include "features/automation/types.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    struct DueItems {
        std::vector<TaskSpec> tasks;
        std::vector<HeartbeatSpec> heartbeats;
    };

    [[nodiscard]]
    std::optional<int> parse_duration_seconds(std::string_view value);
    [[nodiscard]]
    std::optional<base::i64> parse_absolute_time(std::string_view value);
    [[nodiscard]]
    bool is_task_due(const TaskSpec &task, TimePoint now, base::i64 startup_time);
    [[nodiscard]]
    bool is_heartbeat_due(const HeartbeatSpec &heartbeat, TimePoint now);
    [[nodiscard]]
    DueItems collect_due_items(const std::vector<TaskSpec> &tasks, const std::vector<HeartbeatSpec> &heartbeats, TimePoint now, base::i64 startup_time);
    [[nodiscard]]
    std::optional<base::i64> plan_next_heartbeat_due(const HeartbeatSpec &heartbeat, TimePoint from);

} // namespace orangutan::automation
