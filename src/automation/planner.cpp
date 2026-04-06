#include "automation/planner.hpp"

#include "automation/cron-parser.hpp"
#include "types/base.hpp"
#include "utils/local-time.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <random>
#include <string>

namespace orangutan::automation {
    namespace {

        bool parse_integer(std::string_view value, base::i64 &out) {
            auto result = std::from_chars(value.begin(), value.end(), out);
            return result.ec == std::errc{} && result.ptr == value.end();
        }
        std::optional<int> parse_fixed_int(std::string_view value, std::size_t offset, std::size_t width) {
            if (offset + width > value.size()) {
                return std::nullopt;
            }

            int result = 0;
            const auto token = value.substr(offset, width);
            auto [ptr, ec] = std::from_chars(token.begin(), token.end(), result);
            if (ec != std::errc{} || ptr != token.end()) {
                return std::nullopt;
            }
            return result;
        }

        std::optional<std::chrono::local_seconds> parse_local_date_time(std::string_view value) {
            const bool has_seconds = value.size() == 19;
            if (!has_seconds && value.size() != 16) {
                return std::nullopt;
            }

            if (value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' || (has_seconds && value[16] != ':')) {
                return std::nullopt;
            }

            const auto year = parse_fixed_int(value, 0, 4);
            const auto month = parse_fixed_int(value, 5, 2);
            const auto day = parse_fixed_int(value, 8, 2);
            const auto hour = parse_fixed_int(value, 11, 2);
            const auto minute = parse_fixed_int(value, 14, 2);
            const auto second = has_seconds ? parse_fixed_int(value, 17, 2) : std::optional<int>{0};
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value()) {
                return std::nullopt;
            }

            const auto ymd = std::chrono::year{*year} / std::chrono::month{static_cast<unsigned>(*month)} / std::chrono::day{static_cast<unsigned>(*day)};
            if (!ymd.ok() || *hour > 23 || *minute > 59 || *second > 59) {
                return std::nullopt;
            }

            return std::chrono::local_days{ymd} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
        }

        bool within_active_hours(const HeartbeatSpec &heartbeat, base::i64 candidate) {
            if (heartbeat.active_hours.empty()) {
                return true;
            }

            const auto local_time = time::local_time_from(from_unix_seconds(candidate));
            const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
            const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
            const int minute_of_day = static_cast<int>(tod.hours().count() * 60 + tod.minutes().count());

            return std::ranges::any_of(heartbeat.active_hours, [minute_of_day](const ActiveHourWindow &window) {
                return minute_of_day >= window.start_minute && minute_of_day < window.end_minute;
            });
        }

        base::i64 clamp_to_active_hours(const HeartbeatSpec &heartbeat, base::i64 candidate) {
            if (heartbeat.active_hours.empty()) {
                return candidate;
            }

            for (int day_offset = 0; day_offset < 8; ++day_offset) {
                const auto shifted = candidate + (static_cast<base::i64>(day_offset) * 24 * 60 * 60);
                const auto local_time = time::local_time_from(from_unix_seconds(shifted));
                const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
                const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
                const int minute_of_day = static_cast<int>(tod.hours().count() * 60 + tod.minutes().count());

                for (const auto &window : heartbeat.active_hours) {
                    if (day_offset == 0 && minute_of_day >= window.start_minute && minute_of_day < window.end_minute) {
                        return shifted;
                    }
                    if (day_offset > 0 || minute_of_day < window.start_minute) {
                        const auto next_local_time = local_day + std::chrono::hours{window.start_minute / 60} + std::chrono::minutes{window.start_minute % 60};
                        return to_unix_seconds(time::sys_time_from_local(next_local_time));
                    }
                }
            }

            return candidate;
        }

        base::i64 stable_jitter_offset(const HeartbeatSpec &heartbeat, base::i64 base) {
            if (heartbeat.jitter_seconds <= 0) {
                return 0;
            }

            const auto seed = static_cast<base::u64>(std::hash<std::string>{}(heartbeat.id + ":" + std::to_string(base)));
            std::mt19937_64 rng(seed);
            std::uniform_int_distribution<int> dist(-heartbeat.jitter_seconds, heartbeat.jitter_seconds);
            return dist(rng);
        }

    } // namespace

    std::optional<int> parse_duration_seconds(std::string_view value) {
        if (value.empty()) {
            return std::nullopt;
        }

        base::i64 numeric = 0;
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

    std::optional<base::i64> parse_absolute_time(std::string_view value) {
        base::i64 numeric = 0;
        if (parse_integer(value, numeric)) {
            return numeric;
        }

        const auto parsed = parse_local_date_time(value);
        if (!parsed.has_value()) {
            return std::nullopt;
        }

        try {
            return to_unix_seconds(time::sys_time_from_local(*parsed));
        } catch (const std::runtime_error &) {
            return std::nullopt;
        }
    }

    bool is_task_due(const TaskSpec &task, TimePoint now, base::i64 startup_time) {
        if (!task.enabled) {
            return false;
        }

        if (task.schedule.kind == task_schedule_kind::at) {
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

    DueItems collect_due_items(const std::vector<TaskSpec> &tasks, const std::vector<HeartbeatSpec> &heartbeats, TimePoint now, base::i64 startup_time) {
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

    std::optional<base::i64> plan_next_heartbeat_due(const HeartbeatSpec &heartbeat, TimePoint from) {
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
