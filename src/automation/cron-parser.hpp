#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace orangutan::automation {

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

    [[nodiscard]]
    std::expected<CronExpr, std::string> parse_cron_expression(std::string_view expr);

    std::optional<CronExpr> parse_cron(std::string_view expr);
    std::optional<CronExpr> parse_cron_silent(std::string_view expr);

    bool cron_matches(const CronExpr &expr, const TimePoint &time);

    std::optional<TimePoint> next_fire_time(const CronExpr &expr, const TimePoint &after);

} // namespace orangutan::automation

namespace orangutan {

    using automation::cron_matches;
    using automation::CronExpr;
    using automation::CronField;
    using automation::next_fire_time;
    using automation::parse_cron;
    using automation::parse_cron_silent;
    using automation::TimePoint;

} // namespace orangutan
