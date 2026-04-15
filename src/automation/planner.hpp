#pragma once

#include "automation/model.hpp"
#include "automation/automation-types.hpp"

#include <span>
#include <optional>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    struct DueAutomation {
        Automation automation;
        base::i64 scheduled_for = 0;
    };

    [[nodiscard]]
    std::optional<base::i64> plan_next_due(const Automation &automation, TimePoint from);

    [[nodiscard]]
    bool is_automation_due(const Automation &automation, TimePoint now);

    [[nodiscard]]
    std::vector<DueAutomation> collect_due_automations(std::span<const Automation> automations, TimePoint now);

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
