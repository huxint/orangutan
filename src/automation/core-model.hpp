#pragma once

#include "automation/model.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace orangutan::automation {

    struct JobId {
        std::string value;

        auto operator==(const JobId &) const -> bool = default;
    };

    struct ExecutionId {
        std::string value;

        auto operator==(const ExecutionId &) const -> bool = default;
    };

    struct CronSchedule {
        std::string expr;
        std::string time_zone = "UTC";

        auto operator==(const CronSchedule &) const -> bool = default;
    };

    struct IntervalSchedule {
        std::chrono::seconds every{0};
        std::chrono::seconds jitter{0};
        std::vector<ActiveWindow> active_windows;
        std::string time_zone = "UTC";

        auto operator==(const IntervalSchedule &) const -> bool = default;
    };

    struct OneShotSchedule {
        std::chrono::system_clock::time_point at{};

        auto operator==(const OneShotSchedule &) const -> bool = default;
    };

    using ScheduleSpec = std::variant<CronSchedule, IntervalSchedule, OneShotSchedule>;

    [[nodiscard]]
    inline auto to_schedule_spec(const TriggerDefinition &trigger) -> ScheduleSpec {
        switch (trigger.type) {
            case trigger_type::cron:
                return CronSchedule{
                    .expr = trigger.cron,
                    .time_zone = trigger.time_zone,
                };
            case trigger_type::interval:
                return IntervalSchedule{
                    .every = trigger.every,
                    .jitter = trigger.jitter,
                    .active_windows = trigger.active_windows,
                    .time_zone = trigger.time_zone,
                };
            case trigger_type::once:
                return OneShotSchedule{
                    .at = trigger.at,
                };
        }

        std::unreachable();
    }

    enum class MissedRunPolicy : std::uint8_t {
        skip,
        run_once,
        catch_up,
    };

    enum class OverlapPolicy : std::uint8_t {
        forbid,
        queue_one,
        parallel,
    };

    struct ExecutionPolicy {
        MissedRunPolicy missed_runs = MissedRunPolicy::run_once;
        int max_retry_attempts = 0;
        std::chrono::milliseconds initial_backoff{0};
        std::chrono::milliseconds max_backoff{0};
        bool allow_parallel = false;
        OverlapPolicy overlap = OverlapPolicy::forbid;

        auto operator==(const ExecutionPolicy &) const -> bool = default;
    };

    struct ActionDescriptor {
        std::string action_key;
        nlohmann::json payload = nlohmann::json::object();

        auto operator==(const ActionDescriptor &) const -> bool = default;
    };

    struct ResultPolicy {
        delivery_mode mode = delivery_mode::silent;
        std::vector<std::string> targets;
        bool persist_full_reply = true;

        auto operator==(const ResultPolicy &) const -> bool = default;
    };

    struct JobDefinition {
        JobId id;
        std::string key;
        ScheduleSpec schedule;
        ActionDescriptor action;
        ExecutionPolicy execution;
        ResultPolicy result;
        nlohmann::json metadata = nlohmann::json::object();
        std::int64_t version = 0;

        auto operator==(const JobDefinition &) const -> bool = default;
    };

    struct ScheduleState {
        bool enabled = true;
        bool paused = false;
        std::optional<std::int64_t> next_due_at;
        std::optional<std::int64_t> last_scheduled_at;
        std::optional<std::int64_t> last_started_at;
        std::optional<std::int64_t> last_finished_at;
        std::string last_status;
        int in_flight_count = 0;
        std::string lease_owner;
        std::optional<std::int64_t> lease_expires_at;
        std::int64_t revision = 0;

        auto operator==(const ScheduleState &) const -> bool = default;
    };

    enum class DispatchReason : std::uint8_t {
        scheduled,
        catch_up,
        manual,
        resumed,
    };

    struct DispatchRequest {
        JobId job_id;
        ExecutionId execution_id;
        std::chrono::system_clock::time_point scheduled_for{};
        DispatchReason reason = DispatchReason::scheduled;
        ActionDescriptor action;
        ExecutionPolicy execution;
        ResultPolicy result;

        auto operator==(const DispatchRequest &) const -> bool = default;
    };

    struct ExecutionRecord {
        ExecutionId execution_id;
        JobId job_id;
        std::int64_t scheduled_for = 0;
        std::int64_t started_at = 0;
        std::optional<std::int64_t> finished_at;
        std::string status;
        std::string summary;
        std::string reply_ref;
        std::string delivery_status;
        std::string driver_id;

        auto operator==(const ExecutionRecord &) const -> bool = default;
    };

} // namespace orangutan::automation
