#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string_view>

namespace orangutan {

    struct CronField {
        std::set<int> values;
        bool wildcard = false;

        [[nodiscard]]
        bool matches(int value) const {
            return wildcard || values.contains(value);
        }
    };

    struct CronExpr {
        CronField minute;
        CronField hour;
        CronField day_of_month;
        CronField month;
        CronField day_of_week;
    };

    using TimePoint = std::chrono::system_clock::time_point;

    std::optional<CronExpr> parse_cron(std::string_view expr);

    bool cron_matches(const CronExpr &expr, const TimePoint &time);

    std::optional<TimePoint> next_fire_time(const CronExpr &expr, const TimePoint &after);

} // namespace orangutan
