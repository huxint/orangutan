#include "features/automation/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <random>
#include <string_view>

namespace orangutan::automation {
namespace {

std::string random_hex(std::size_t count) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    std::string value;
    value.reserve(count);
    std::uniform_int_distribution<int> dist(0, 15);
    for (std::size_t index = 0; index < count; ++index) {
        value.push_back(hex.at(static_cast<std::size_t>(dist(rng))));
    }
    return value;
}

} // namespace

std::string generate_id(std::string_view prefix) {
    const auto now = to_unix_seconds(Clock::now());
    return std::string(prefix) + "-" + std::to_string(now) + "-" + random_hex(10);
}

std::int64_t to_unix_seconds(TimePoint time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

TimePoint from_unix_seconds(std::int64_t seconds) {
    return TimePoint{std::chrono::seconds(seconds)};
}

std::string kind_to_string(Kind kind) {
    switch (kind) {
        case Kind::task:
            return "task";
        case Kind::heartbeat:
            return "heartbeat";
    }
    return "task";
}

std::optional<Kind> kind_from_string(std::string_view value) {
    if (value == "task") {
        return Kind::task;
    }
    if (value == "heartbeat") {
        return Kind::heartbeat;
    }
    return std::nullopt;
}

std::string delivery_mode_to_string(DeliveryMode mode) {
    switch (mode) {
        case DeliveryMode::silent:
            return "silent";
        case DeliveryMode::notify:
            return "notify";
    }
    return "silent";
}

std::optional<DeliveryMode> delivery_mode_from_string(std::string_view value) {
    if (value == "silent") {
        return DeliveryMode::silent;
    }
    if (value == "notify") {
        return DeliveryMode::notify;
    }
    return std::nullopt;
}

std::string task_schedule_kind_to_string(TaskScheduleKind kind) {
    switch (kind) {
        case TaskScheduleKind::at:
            return "at";
        case TaskScheduleKind::cron:
            return "cron";
    }
    return "cron";
}

std::optional<TaskScheduleKind> task_schedule_kind_from_string(std::string_view value) {
    if (value == "at") {
        return TaskScheduleKind::at;
    }
    if (value == "cron") {
        return TaskScheduleKind::cron;
    }
    return std::nullopt;
}

json delivery_policy_to_json(const DeliveryPolicy &delivery) {
    return {
        {"mode", delivery_mode_to_string(delivery.mode)},
        {"targets", delivery.targets},
    };
}

DeliveryPolicy delivery_policy_from_json(const json &value) {
    DeliveryPolicy delivery;
    if (!value.is_object()) {
        return delivery;
    }

    if (const auto it = value.find("mode"); it != value.end() && it->is_string()) {
        if (const auto parsed = delivery_mode_from_string(it->get<std::string>()); parsed.has_value()) {
            delivery.mode = *parsed;
        }
    }

    if (const auto it = value.find("targets"); it != value.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (item.is_string()) {
                delivery.targets.push_back(item.get<std::string>());
            }
        }
    }

    return delivery;
}

json active_hours_to_json(const std::vector<ActiveHourWindow> &windows) {
    json result = json::array();
    for (const auto &window : windows) {
        result.push_back({
            {"start_minute", window.start_minute},
            {"end_minute", window.end_minute},
        });
    }
    return result;
}

std::vector<ActiveHourWindow> active_hours_from_json(const json &value) {
    std::vector<ActiveHourWindow> windows;
    if (!value.is_array()) {
        return windows;
    }

    for (const auto &item : value) {
        if (!item.is_object()) {
            continue;
        }

        ActiveHourWindow window;
        window.start_minute = item.value("start_minute", 0);
        window.end_minute = item.value("end_minute", 24 * 60);
        window.start_minute = std::max(window.start_minute, 0);
        window.end_minute = std::min(window.end_minute, 24 * 60);
        if (window.start_minute >= window.end_minute) {
            continue;
        }
        windows.push_back(window);
    }

    return windows;
}

} // namespace orangutan::automation
