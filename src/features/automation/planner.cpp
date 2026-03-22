#include "features/automation/planner.hpp"

#include "features/cron/parser.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace orangutan::automation {
namespace {

bool parse_integer(std::string_view value, long long &out) {
    auto result = std::from_chars(value.begin(), value.end(), out);
    return result.ec == std::errc{} && result.ptr == value.end();
}

bool within_active_hours(const HeartbeatSpec &heartbeat, std::int64_t candidate) {
    if (heartbeat.active_hours.empty()) {
        return true;
    }

    const auto time_value = static_cast<std::time_t>(candidate);
    std::tm local_tm{};
    localtime_r(&time_value, &local_tm);
    const int minute_of_day = (local_tm.tm_hour * 60) + local_tm.tm_min;

    return std::ranges::any_of(heartbeat.active_hours, [minute_of_day](const ActiveHourWindow &window) {
        return minute_of_day >= window.start_minute && minute_of_day < window.end_minute;
    });
}

std::int64_t clamp_to_active_hours(const HeartbeatSpec &heartbeat, std::int64_t candidate) {
    if (heartbeat.active_hours.empty()) {
        return candidate;
    }

    for (int day_offset = 0; day_offset < 8; ++day_offset) {
        const auto shifted = candidate + (static_cast<std::int64_t>(day_offset) * 24 * 60 * 60);
        const auto shifted_time = static_cast<std::time_t>(shifted);
        std::tm local_tm{};
        localtime_r(&shifted_time, &local_tm);
        const int minute_of_day = (local_tm.tm_hour * 60) + local_tm.tm_min;

        for (const auto &window : heartbeat.active_hours) {
            if (day_offset == 0 && minute_of_day >= window.start_minute && minute_of_day < window.end_minute) {
                return shifted;
            }
            if (day_offset > 0 || minute_of_day < window.start_minute) {
                local_tm.tm_hour = window.start_minute / 60;
                local_tm.tm_min = window.start_minute % 60;
                local_tm.tm_sec = 0;
                return static_cast<std::int64_t>(std::mktime(&local_tm));
            }
        }
    }

    return candidate;
}

std::int64_t stable_jitter_offset(const HeartbeatSpec &heartbeat, std::int64_t base) {
    if (heartbeat.jitter_seconds <= 0) {
        return 0;
    }

    const auto seed = static_cast<std::uint64_t>(std::hash<std::string>{}(heartbeat.id + ":" + std::to_string(base)));
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> dist(-heartbeat.jitter_seconds, heartbeat.jitter_seconds);
    return dist(rng);
}

} // namespace

std::optional<int> parse_duration_seconds(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    long long numeric = 0;
    if (parse_integer(value, numeric)) {
        if (numeric <= 0) {
            return std::nullopt;
        }
        return static_cast<int>(numeric);
    }

    const char suffix = value.back();
    auto number_part = value.substr(0, value.size() - 1);
    if (!parse_integer(number_part, numeric) || numeric <= 0) {
        return std::nullopt;
    }

    switch (suffix) {
        case 's':
            return static_cast<int>(numeric);
        case 'm':
            return static_cast<int>(numeric * 60);
        case 'h':
            return static_cast<int>(numeric * 60 * 60);
        case 'd':
            return static_cast<int>(numeric * 24 * 60 * 60);
        default:
            return std::nullopt;
    }
}

std::optional<std::int64_t> parse_absolute_time(std::string_view value) {
    long long numeric = 0;
    if (parse_integer(value, numeric)) {
        return static_cast<std::int64_t>(numeric);
    }

    std::tm tm{};
    std::istringstream stream{std::string(value)};
    stream >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (!stream.fail()) {
        return static_cast<std::int64_t>(std::mktime(&tm));
    }

    stream.clear();
    stream.str(std::string(value));
    stream >> std::get_time(&tm, "%Y-%m-%d %H:%M");
    if (!stream.fail()) {
        return static_cast<std::int64_t>(std::mktime(&tm));
    }

    return std::nullopt;
}

bool is_task_due(const TaskSpec &task, TimePoint now, std::int64_t startup_time) {
    if (!task.enabled) {
        return false;
    }

    if (task.schedule.kind == TaskScheduleKind::at) {
        const auto scheduled = parse_absolute_time(task.schedule.value);
        if (!scheduled.has_value() || *scheduled < startup_time) {
            return false;
        }
        if (task.last_run_at.has_value() && *task.last_run_at >= *scheduled) {
            return false;
        }
        return *scheduled <= to_unix_seconds(now);
    }

    const auto expr = parse_cron(task.schedule.value);
    if (!expr.has_value()) {
        return false;
    }

    const auto current_minute = std::chrono::floor<std::chrono::minutes>(now);
    if (!cron_matches(*expr, current_minute)) {
        return false;
    }
    if (to_unix_seconds(current_minute) < startup_time) {
        return false;
    }

    if (!task.last_run_at.has_value()) {
        return true;
    }

    const auto last_minute = std::chrono::floor<std::chrono::minutes>(from_unix_seconds(*task.last_run_at));
    return last_minute != current_minute;
}

bool is_heartbeat_due(const HeartbeatSpec &heartbeat, TimePoint now) {
    if (!heartbeat.enabled || heartbeat.paused || !heartbeat.next_due_at.has_value()) {
        return false;
    }
    return *heartbeat.next_due_at <= to_unix_seconds(now);
}

DueItems collect_due_items(const std::vector<TaskSpec> &tasks, const std::vector<HeartbeatSpec> &heartbeats, TimePoint now, std::int64_t startup_time) {
    DueItems due;
    for (const auto &task : tasks) {
        if (is_task_due(task, now, startup_time)) {
            due.tasks.push_back(task);
        }
    }
    for (const auto &heartbeat : heartbeats) {
        if (is_heartbeat_due(heartbeat, now)) {
            due.heartbeats.push_back(heartbeat);
        }
    }
    return due;
}

std::optional<std::int64_t> plan_next_heartbeat_due(const HeartbeatSpec &heartbeat, TimePoint from) {
    if (heartbeat.every_seconds <= 0) {
        return std::nullopt;
    }

    auto base = to_unix_seconds(from) + heartbeat.every_seconds;
    base += stable_jitter_offset(heartbeat, base);
    if (!within_active_hours(heartbeat, base)) {
        base = clamp_to_active_hours(heartbeat, base);
    }
    return base;
}

} // namespace orangutan::automation
