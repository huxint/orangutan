#include "automation/cron-parser.hpp"
#include "utils/local-time.hpp"

#include <charconv>
#include <ranges>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    namespace {

        std::optional<int> parse_int_token(std::string_view token) {
            if (token.empty()) {
                return std::nullopt;
            }

            int value = 0;
            auto [ptr, ec] = std::from_chars(token.begin(), token.end(), value);
            if (ec != std::errc{} || ptr != token.end()) {
                return std::nullopt;
            }

            return value;
        }

        bool in_bounds(int value, int min_val, int max_val) {
            return value >= min_val && value <= max_val;
        }

        std::optional<CronField> parse_field(std::string_view token, int min_val, int max_val) {
            CronField field;

            if (token == "*") {
                field.wildcard = true;
                return field;
            }

            if (token.starts_with("*/")) {
                auto step = parse_int_token(token.substr(2));
                if (!step || *step <= 0) {
                    return std::nullopt;
                }
                for (int v = min_val; v <= max_val; v += *step) {
                    field.values.insert(v);
                }
                return field;
            }

            for (auto subrange : token | std::views::split(',')) {
                const std::string_view part{subrange};
                const auto dash = part.find('-');
                const auto slash = part.find('/');

                if (dash != std::string_view::npos) {
                    const auto range_str = (slash != std::string_view::npos) ? part.substr(0, slash) : part;
                    const auto start_str = range_str.substr(0, range_str.find('-'));
                    const auto end_str = range_str.substr(range_str.find('-') + 1);

                    const auto parsed_start = parse_int_token(start_str);
                    const auto parsed_end = parse_int_token(end_str);
                    if (!parsed_start || !parsed_end) {
                        return std::nullopt;
                    }

                    const int range_start = *parsed_start;
                    const int range_end = *parsed_end;
                    if (!in_bounds(range_start, min_val, max_val) || !in_bounds(range_end, min_val, max_val) || range_start > range_end) {
                        return std::nullopt;
                    }

                    int step = 1;
                    if (slash != std::string_view::npos) {
                        const auto parsed_step = parse_int_token(part.substr(slash + 1));
                        if (!parsed_step || *parsed_step <= 0) {
                            return std::nullopt;
                        }
                        step = *parsed_step;
                    }

                    for (int v = range_start; v <= range_end; v += step) {
                        field.values.insert(v);
                    }
                } else {
                    const auto parsed_val = parse_int_token(part);
                    if (!parsed_val || !in_bounds(*parsed_val, min_val, max_val)) {
                        return std::nullopt;
                    }
                    field.values.insert(*parsed_val);
                }
            }

            return field;
        }

        std::vector<std::string_view> split_whitespace(std::string_view sv) {
            std::vector<std::string_view> tokens;
            std::size_t i = 0;
            while (i < sv.size()) {
                while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t')) {
                    ++i;
                }
                if (i >= sv.size()) {
                    break;
                }
                std::size_t start = i;
                while (i < sv.size() && sv[i] != ' ' && sv[i] != '\t') {
                    ++i;
                }
                tokens.push_back(sv.substr(start, i - start));
            }
            return tokens;
        }

        std::optional<CronExpr> parse_cron_impl(std::string_view expr, bool emit_errors) {
            auto tokens = split_whitespace(expr);
            if (tokens.size() != 5) {
                if (emit_errors) {
                    spdlog::error("cron expression must have exactly 5 fields: '{}'", expr);
                }
                return std::nullopt;
            }

            auto minute = parse_field(tokens[0], 0, 59);
            auto hour = parse_field(tokens[1], 0, 23);
            auto dom = parse_field(tokens[2], 1, 31);
            auto month = parse_field(tokens[3], 1, 12);
            auto dow = parse_field(tokens[4], 0, 6);

            if (!minute || !hour || !dom || !month || !dow) {
                if (emit_errors) {
                    spdlog::error("failed to parse cron expression: '{}'", expr);
                }
                return std::nullopt;
            }

            return CronExpr{
                .minute = *minute,
                .hour = *hour,
                .day_of_month = *dom,
                .month = *month,
                .day_of_week = *dow,
            };
        }

    } // namespace

    std::expected<CronExpr, std::string> parse_cron_expression(std::string_view expr) {
        const auto parsed = parse_cron_silent(expr);
        if (!parsed.has_value()) {
            return std::unexpected("cron expression is invalid");
        }
        return *parsed;
    }

    std::optional<CronExpr> parse_cron(std::string_view expr) {
        return parse_cron_impl(expr, true);
    }

    std::optional<CronExpr> parse_cron_silent(std::string_view expr) {
        return parse_cron_impl(expr, false);
    }

    bool cron_matches_local(const CronExpr &expr, std::chrono::local_seconds local_time) {
        const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
        const auto ymd = std::chrono::year_month_day{local_day};
        const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
        const auto day_of_month_matches = expr.day_of_month.matches(static_cast<int>(static_cast<unsigned>(ymd.day())));
        const auto day_of_week_matches = expr.day_of_week.matches(static_cast<int>(std::chrono::weekday{local_day}.c_encoding()));
        const auto either_day_field_restricted = !expr.day_of_month.wildcard || !expr.day_of_week.wildcard;
        const auto both_day_fields_restricted = !expr.day_of_month.wildcard && !expr.day_of_week.wildcard;

        auto day_matches = true;
        if (both_day_fields_restricted) {
            day_matches = day_of_month_matches || day_of_week_matches;
        } else if (either_day_field_restricted) {
            day_matches = day_of_month_matches && day_of_week_matches;
        }

        return expr.minute.matches(static_cast<int>(tod.minutes().count())) && expr.hour.matches(static_cast<int>(tod.hours().count())) &&
               expr.month.matches(static_cast<int>(static_cast<unsigned>(ymd.month()))) && day_matches;
    }

    bool cron_matches(const CronExpr &expr, const TimePoint &time_point) {
        return cron_matches_local(expr, time::local_time_from(time_point));
    }

    std::optional<TimePoint> next_fire_time(const CronExpr &expr, const TimePoint &after) {
        constexpr int MAX_MINUTES = 2 * 366 * 24 * 60;

        const auto start_sys = std::chrono::ceil<std::chrono::minutes>(after + std::chrono::seconds(1));
        auto local_candidate = std::chrono::floor<std::chrono::seconds>(time::local_time_from(start_sys));

        for (int i = 0; i < MAX_MINUTES; ++i) {
            if (cron_matches_local(expr, local_candidate)) {
                return time::sys_time_from_local(local_candidate);
            }
            local_candidate += std::chrono::minutes(1);
        }

        return std::nullopt;
    }

} // namespace orangutan::automation
