#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "types/types.hpp"

namespace orangutan::automation {

    class AutomationBuilder;

    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    enum class delivery_mode : base::u8 {
        silent,
        notify,
    };

    struct DeliveryPolicy {
        delivery_mode mode = delivery_mode::silent;
        std::vector<std::string> targets;
    };

    struct RunRecord {
        std::string id;
        std::string automation_id;
        std::string agent_key;
        std::string automation_name;
        base::i64 started_at = 0;
        std::optional<base::i64> finished_at;
        std::string status;
        std::string summary;
        std::string reply;
        std::string delivery_status;
        std::string log_path;
    };

    struct ExecutionResult {
        bool success = false;
        std::string reply;
        std::string summary;
        std::string workspace_root;
    };

    [[nodiscard]]
    std::string generate_id(std::string_view prefix);

    [[nodiscard]]
    base::i64 to_unix_seconds(TimePoint time);

    [[nodiscard]]
    TimePoint from_unix_seconds(base::i64 seconds);

    [[nodiscard]]
    nlohmann::json delivery_policy_to_json(const DeliveryPolicy &delivery);

    [[nodiscard]]
    DeliveryPolicy delivery_policy_from_json(const nlohmann::json &value);

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
