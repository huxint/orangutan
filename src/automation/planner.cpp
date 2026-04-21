#include "automation/planner.hpp"

#include "automation/cron-parser.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>

namespace orangutan::automation {
    namespace {

        constexpr int MAX_CRON_SCAN_MINUTES = 2 * 366 * 24 * 60;

        [[nodiscard]]
        const std::chrono::time_zone *resolve_time_zone(std::string_view zone_name) {
            if (zone_name.empty() || zone_name == "UTC") {
                return std::chrono::locate_zone("UTC");
            }
            return std::chrono::locate_zone(std::string(zone_name));
        }

        [[nodiscard]]
        std::chrono::local_seconds local_time_in_zone(TimePoint time_point, std::string_view zone_name) {
            const auto zone = resolve_time_zone(zone_name);
            return std::chrono::zoned_time{zone, std::chrono::floor<std::chrono::seconds>(time_point)}.get_local_time();
        }

        [[nodiscard]]
        std::optional<TimePoint> next_cron_fire_time(const TriggerDefinition &trigger, TimePoint after) {
            const auto parsed = parse_cron_silent(trigger.cron);
            if (!parsed.has_value()) {
                return std::nullopt;
            }

            auto candidate = std::chrono::ceil<std::chrono::minutes>(after + std::chrono::seconds{1});
            for (int index = 0; index < MAX_CRON_SCAN_MINUTES; ++index) {
                if (cron_matches_local(*parsed, local_time_in_zone(candidate, trigger.time_zone))) {
                    return candidate;
                }
                candidate += std::chrono::minutes{1};
            }

            return std::nullopt;
        }

        [[nodiscard]]
        std::int64_t positive_jitter_offset(const Automation &automation, TimePoint base_time) {
            if (automation.trigger.jitter <= std::chrono::seconds{0}) {
                return 0;
            }

            const auto seed_material = automation.id + ":" + std::to_string(to_unix_seconds(base_time));
            std::mt19937_64 generator(static_cast<std::uint64_t>(std::hash<std::string>{}(seed_material)));
            std::uniform_int_distribution<std::int64_t> distribution(0, automation.trigger.jitter.count());
            return distribution(generator);
        }

        [[nodiscard]]
        bool within_active_windows(const TriggerDefinition &trigger, TimePoint candidate) {
            if (trigger.active_windows.empty()) {
                return true;
            }

            const auto local_time = local_time_in_zone(candidate, trigger.time_zone);
            const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
            const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
            const auto minute_of_day = std::chrono::minutes{tod.hours()} + std::chrono::minutes{tod.minutes()};

            return std::ranges::any_of(trigger.active_windows, [minute_of_day](const ActiveWindow &window) {
                return minute_of_day >= window.start && minute_of_day < window.end;
            });
        }

        [[nodiscard]]
        TimePoint clamp_to_active_windows(const TriggerDefinition &trigger, TimePoint candidate) {
            if (trigger.active_windows.empty() || within_active_windows(trigger, candidate)) {
                return candidate;
            }

            auto windows = trigger.active_windows;
            std::ranges::sort(windows, [](const ActiveWindow &lhs, const ActiveWindow &rhs) {
                return lhs.start < rhs.start;
            });

            const auto zone = resolve_time_zone(trigger.time_zone);
            const auto local_candidate = local_time_in_zone(candidate, trigger.time_zone);
            const auto local_day = std::chrono::floor<std::chrono::days>(local_candidate);
            const auto tod = std::chrono::hh_mm_ss{local_candidate - local_day};
            const auto minute_of_day = std::chrono::minutes{tod.hours()} + std::chrono::minutes{tod.minutes()};

            for (int day_offset = 0; day_offset < 8; ++day_offset) {
                const auto candidate_day = local_day + std::chrono::days{day_offset};
                for (const auto &window : windows) {
                    if (day_offset == 0 && minute_of_day < window.start) {
                        const auto next_local = std::chrono::time_point_cast<std::chrono::seconds>(candidate_day + window.start);
                        return std::chrono::time_point_cast<Clock::duration>(zone->to_sys(next_local, std::chrono::choose::latest));
                    }
                    if (day_offset > 0) {
                        const auto next_local = std::chrono::time_point_cast<std::chrono::seconds>(candidate_day + window.start);
                        return std::chrono::time_point_cast<Clock::duration>(zone->to_sys(next_local, std::chrono::choose::latest));
                    }
                }
            }

            return candidate;
        }

        [[nodiscard]]
        std::optional<TimePoint> plan_interval_due_time(const Automation &automation, TimePoint from) {
            if (automation.trigger.every <= std::chrono::seconds{0}) {
                return std::nullopt;
            }

            auto candidate = from + automation.trigger.every + std::chrono::seconds{positive_jitter_offset(automation, from)};
            candidate = clamp_to_active_windows(automation.trigger, candidate);
            return candidate;
        }

        [[nodiscard]]
        std::optional<TimePoint> plan_due_time(const Automation &automation, TimePoint from) {
            switch (automation.trigger.type) {
            case trigger_type::cron:
                return next_cron_fire_time(automation.trigger, from);
            case trigger_type::interval:
                return plan_interval_due_time(automation, from);
            case trigger_type::once:
                if (automation.last_run_at.has_value()) {
                    return std::nullopt;
                }
                return automation.trigger.at;
            }

            return std::nullopt;
        }

        [[nodiscard]]
        std::int64_t scheduled_for_time(const Automation &automation, TimePoint now) {
            if (automation.next_due_at.has_value()) {
                return *automation.next_due_at;
            }

            if (automation.trigger.type == trigger_type::once && !automation.last_run_at.has_value()) {
                return to_unix_seconds(automation.trigger.at);
            }

            return to_unix_seconds(now);
        }

    } // namespace

    std::optional<std::int64_t> plan_next_due(const Automation &automation, TimePoint from) {
        if (!automation.enabled || automation.paused) {
            return std::nullopt;
        }

        const auto next_due = plan_due_time(automation, from);
        if (!next_due.has_value()) {
            return std::nullopt;
        }
        return to_unix_seconds(*next_due);
    }

    bool is_automation_due(const Automation &automation, TimePoint now) {
        if (!automation.enabled || automation.paused) {
            return false;
        }

        if (automation.next_due_at.has_value()) {
            return *automation.next_due_at <= to_unix_seconds(now);
        }

        if (automation.trigger.type == trigger_type::once && !automation.last_run_at.has_value()) {
            return automation.trigger.at <= now;
        }

        return false;
    }

    std::vector<DueAutomation> collect_due_automations(std::span<const Automation> automations, TimePoint now) {
        std::vector<DueAutomation> due;
        for (const auto &automation : automations) {
            if (!is_automation_due(automation, now)) {
                continue;
            }

            due.push_back({
                .automation = automation,
                .scheduled_for = scheduled_for_time(automation, now),
            });
        }

        std::ranges::sort(due, [](const DueAutomation &lhs, const DueAutomation &rhs) {
            if (lhs.scheduled_for != rhs.scheduled_for) {
                return lhs.scheduled_for < rhs.scheduled_for;
            }
            if (lhs.automation.agent_key != rhs.automation.agent_key) {
                return lhs.automation.agent_key < rhs.automation.agent_key;
            }
            return lhs.automation.id < rhs.automation.id;
        });
        return due;
    }

} // namespace orangutan::automation
