#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "automation/automation-types.hpp"

namespace orangutan::automation {

    class AutomationBuilder;

    /// Represents the supported unified trigger kinds for automations.
    enum class trigger_type : base::u8 {
        cron,
        interval,
        once,
    };

    /// Represents one active time window for interval automations.
    struct ActiveWindow {
        std::chrono::minutes start = std::chrono::minutes{0};
        std::chrono::minutes end = std::chrono::hours{24};
    };

    /// Represents one concrete trigger definition attached to an automation.
    struct TriggerDefinition {
        trigger_type type = trigger_type::cron;
        std::chrono::seconds every = std::chrono::seconds{0};
        std::chrono::seconds jitter = std::chrono::seconds{0};
        TimePoint at{};
        std::string cron;
        std::string time_zone = "UTC";
        std::vector<ActiveWindow> active_windows;
    };

    /// Represents one unified automation definition and its runtime state.
    struct Automation {
        std::string id;
        std::string agent_key;
        std::string name;
        std::string prompt;
        std::string notes;
        TriggerDefinition trigger;
        DeliveryPolicy delivery;
        std::vector<std::string> tags;
        std::optional<base::i64> last_run_at;
        std::optional<base::i64> next_due_at;
        std::string last_status;
        bool enabled = true;
        bool paused = false;

        [[nodiscard]]
        static AutomationBuilder named(std::string_view name);
    };

} // namespace orangutan::automation
