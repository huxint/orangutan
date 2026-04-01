#pragma once

#include "types/types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    enum class Kind {
        task,
        heartbeat,
    };

    enum class DeliveryMode {
        silent,
        notify,
    };

    enum class TaskScheduleKind {
        at,
        cron,
    };

    struct DeliveryPolicy {
        DeliveryMode mode = DeliveryMode::silent;
        std::vector<std::string> targets;
    };

    struct TaskSchedule {
        TaskScheduleKind kind = TaskScheduleKind::cron;
        std::string value;
    };

    struct ActiveHourWindow {
        int start_minute = 0;
        int end_minute = 24 * 60;
    };

    struct TaskSpec {
        std::string id;
        std::string agent_key;
        std::string name;
        bool enabled = true;
        TaskSchedule schedule;
        std::string prompt;
        std::string notes;
        DeliveryPolicy delivery;
        std::optional<base::i64> last_run_at;
        std::string last_status;
    };

    struct HeartbeatSpec {
        std::string id;
        std::string agent_key;
        std::string name;
        bool enabled = true;
        int every_seconds = 30 * 60;
        int jitter_seconds = 0;
        std::vector<ActiveHourWindow> active_hours;
        std::string prompt;
        std::string notes;
        DeliveryPolicy delivery;
        bool paused = false;
        std::optional<base::i64> next_due_at;
        std::optional<base::i64> last_run_at;
        std::string last_status;
    };

    struct RunRecord {
        std::string id;
        Kind kind = Kind::task;
        std::string automation_id;
        std::string agent_key;
        std::string automation_name;
        base::i64 started_at = 0;
        std::optional<base::i64> finished_at;
        std::string status;
        std::string summary;
        std::string delivery_status;
        std::string log_path;
    };

    struct InboxItem {
        std::string id;
        std::string agent_key;
        std::string source_kind;
        std::string source_run_id;
        std::string title;
        std::string body;
        base::i64 created_at = 0;
        std::optional<base::i64> acked_at;
        std::string status = "unread";
    };

    struct Trigger {
        Kind kind = Kind::task;
        std::string automation_id;
        std::string agent_key;
        std::string name;
        std::string prompt;
        DeliveryPolicy delivery;
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
    [[nodiscard]]
    nlohmann::json active_hours_to_json(const std::vector<ActiveHourWindow> &windows);
    [[nodiscard]]
    std::vector<ActiveHourWindow> active_hours_from_json(const nlohmann::json &value);

} // namespace orangutan::automation
