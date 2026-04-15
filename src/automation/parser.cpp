#include "automation/parser.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <expected>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "automation/cron-parser.hpp"

namespace orangutan::automation {
    namespace {

        constexpr auto FULL_DAY_MINUTES = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours{24});

        [[nodiscard]]
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

        [[nodiscard]]
        std::expected<std::string, std::string> parse_required_string_field(const nlohmann::json &value, std::string_view field_name) {
            const auto field = value.find(std::string(field_name));
            if (field == value.end()) {
                return std::unexpected(std::string(field_name) + " is required");
            }
            if (!field->is_string()) {
                return std::unexpected(std::string(field_name) + " must be a string");
            }
            return field->get<std::string>();
        }

        [[nodiscard]]
        std::expected<std::string, std::string> parse_time_zone_field(const nlohmann::json &value) {
            const auto field = value.find("time_zone");
            if (field == value.end()) {
                return std::string("UTC");
            }
            if (!field->is_string()) {
                return std::unexpected("time_zone must be a string");
            }

            const auto parsed = field->get<std::string>();
            const auto is_blank = std::ranges::all_of(parsed, [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
            if (parsed.empty() || is_blank) {
                return std::unexpected("time_zone must not be empty");
            }
            return parsed;
        }

        [[nodiscard]]
        std::string format_duration_string(std::chrono::seconds value) {
            const auto total_seconds = value.count();
            if (total_seconds != 0 && total_seconds % (24 * 60 * 60) == 0) {
                return std::to_string(total_seconds / (24 * 60 * 60)) + "d";
            }
            if (total_seconds != 0 && total_seconds % (60 * 60) == 0) {
                return std::to_string(total_seconds / (60 * 60)) + "h";
            }
            if (total_seconds != 0 && total_seconds % 60 == 0) {
                return std::to_string(total_seconds / 60) + "m";
            }
            return std::to_string(total_seconds) + "s";
        }

        [[nodiscard]]
        std::string format_time_of_day(std::chrono::minutes value) {
            const auto total_minutes = value.count();
            const auto hours = total_minutes / 60;
            const auto minutes = total_minutes % 60;

            std::ostringstream stream;
            stream << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes;
            return stream.str();
        }

        [[nodiscard]]
        std::expected<std::chrono::minutes, std::string> parse_time_of_day(std::string_view value) {
            if (value.size() != 5 || value[2] != ':') {
                return std::unexpected("time values must use hh:mm");
            }

            const auto hour = parse_fixed_int(value, 0, 2);
            const auto minute = parse_fixed_int(value, 3, 2);
            if (!hour.has_value() || !minute.has_value()) {
                return std::unexpected("time values must use hh:mm");
            }
            if (*minute > 59) {
                return std::unexpected("time values must use valid hours and minutes");
            }
            if (*hour == 24 && *minute == 0) {
                return FULL_DAY_MINUTES;
            }
            if (*hour < 0 || *hour > 23) {
                return std::unexpected("time values must use valid hours and minutes");
            }

            return std::chrono::hours{*hour} + std::chrono::minutes{*minute};
        }

        [[nodiscard]]
        std::string format_iso_utc(TimePoint value) {
            const auto truncated = std::chrono::floor<std::chrono::seconds>(value);
            const auto day = std::chrono::floor<std::chrono::days>(truncated);
            const auto ymd = std::chrono::year_month_day{day};
            const auto tod = std::chrono::hh_mm_ss{truncated - day};

            std::ostringstream stream;
            stream << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-' << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
                   << static_cast<unsigned>(ymd.day()) << 'T' << std::setw(2) << tod.hours().count() << ':' << std::setw(2) << tod.minutes().count() << ':' << std::setw(2)
                   << tod.seconds().count() << 'Z';
            return stream.str();
        }

        [[nodiscard]]
        std::expected<TimePoint, std::string> parse_iso_utc(std::string_view value) {
            if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
                return std::unexpected("at must use iso-8601 utc format");
            }

            const auto year = parse_fixed_int(value, 0, 4);
            const auto month = parse_fixed_int(value, 5, 2);
            const auto day = parse_fixed_int(value, 8, 2);
            const auto hour = parse_fixed_int(value, 11, 2);
            const auto minute = parse_fixed_int(value, 14, 2);
            const auto second = parse_fixed_int(value, 17, 2);
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value()) {
                return std::unexpected("at must use iso-8601 utc format");
            }
            if (*hour > 23 || *minute > 59 || *second > 59) {
                return std::unexpected("at must use valid utc time values");
            }

            const auto ymd = std::chrono::year{*year} / std::chrono::month{static_cast<unsigned>(*month)} / std::chrono::day{static_cast<unsigned>(*day)};
            if (!ymd.ok()) {
                return std::unexpected("at must use a valid utc date");
            }

            const auto utc_time = std::chrono::sys_days{ymd} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
            return std::chrono::time_point_cast<Clock::duration>(utc_time);
        }

        [[nodiscard]]
        std::expected<ActiveWindow, std::string> parse_active_window(const nlohmann::json &value) {
            if (!value.is_object()) {
                return std::unexpected("active_windows entries must be objects");
            }

            const auto start_value = parse_required_string_field(value, "start");
            if (!start_value.has_value()) {
                return std::unexpected(start_value.error());
            }

            const auto end_value = parse_required_string_field(value, "end");
            if (!end_value.has_value()) {
                return std::unexpected(end_value.error());
            }

            const auto start = parse_time_of_day(*start_value);
            if (!start.has_value()) {
                return std::unexpected(start.error());
            }

            const auto end = parse_time_of_day(*end_value);
            if (!end.has_value()) {
                return std::unexpected(end.error());
            }

            if (*start >= *end) {
                return std::unexpected("active window start must be before end");
            }

            return ActiveWindow{
                .start = *start,
                .end = *end,
            };
        }

    } // namespace

    nlohmann::json trigger_to_json(const TriggerDefinition &trigger) {
        switch (trigger.type) {
        case trigger_type::cron:
            return {
                {"type", "cron"},
                {"cron", trigger.cron},
                {"time_zone", trigger.time_zone.empty() ? "UTC" : trigger.time_zone},
            };
        case trigger_type::interval: {
            nlohmann::json active_windows = nlohmann::json::array();
            for (const auto &window : trigger.active_windows) {
                active_windows.push_back({
                    {"start", format_time_of_day(window.start)},
                    {"end", format_time_of_day(window.end)},
                });
            }

            return {
                {"type", "interval"},
                {"every", format_duration_string(trigger.every)},
                {"jitter", format_duration_string(trigger.jitter)},
                {"time_zone", trigger.time_zone.empty() ? "UTC" : trigger.time_zone},
                {"active_windows", std::move(active_windows)},
            };
        }
        case trigger_type::once:
            return {
                {"type", "once"},
                {"at", format_iso_utc(trigger.at)},
            };
        }

        return nlohmann::json::object();
    }

    std::expected<TriggerDefinition, std::string> trigger_from_json(const nlohmann::json &value) {
        if (!value.is_object()) {
            return std::unexpected("trigger must be an object");
        }

        const auto type_value = parse_required_string_field(value, "type");
        if (!type_value.has_value()) {
            return std::unexpected(type_value.error());
        }

        if (*type_value == "cron") {
            const auto cron_value = parse_required_string_field(value, "cron");
            if (!cron_value.has_value()) {
                return std::unexpected(cron_value.error());
            }

            const auto parsed_cron = parse_cron_expression(*cron_value);
            if (!parsed_cron.has_value()) {
                return std::unexpected(parsed_cron.error());
            }

            const auto time_zone = parse_time_zone_field(value);
            if (!time_zone.has_value()) {
                return std::unexpected(time_zone.error());
            }

            return TriggerDefinition{
                .type = trigger_type::cron,
                .cron = *cron_value,
                .time_zone = *time_zone,
            };
        }

        if (*type_value == "interval") {
            const auto every_value = parse_required_string_field(value, "every");
            if (!every_value.has_value()) {
                return std::unexpected(every_value.error());
            }

            const auto every = parse_duration_string(*every_value);
            if (!every.has_value()) {
                return std::unexpected(every.error());
            }
            if (*every <= std::chrono::seconds{0}) {
                return std::unexpected("every must be greater than zero");
            }

            std::chrono::seconds jitter = std::chrono::seconds{0};
            const auto jitter_field = value.find("jitter");
            if (jitter_field != value.end()) {
                if (!jitter_field->is_string()) {
                    return std::unexpected("jitter must be a string");
                }

                const auto parsed_jitter = parse_duration_string(jitter_field->get_ref<const std::string &>());
                if (!parsed_jitter.has_value()) {
                    return std::unexpected(parsed_jitter.error());
                }
                jitter = *parsed_jitter;
            }

            const auto time_zone = parse_time_zone_field(value);
            if (!time_zone.has_value()) {
                return std::unexpected(time_zone.error());
            }

            std::vector<ActiveWindow> active_windows;
            const auto active_windows_field = value.find("active_windows");
            if (active_windows_field != value.end()) {
                if (!active_windows_field->is_array()) {
                    return std::unexpected("active_windows must be an array");
                }

                for (const auto &window_value : *active_windows_field) {
                    const auto parsed_window = parse_active_window(window_value);
                    if (!parsed_window.has_value()) {
                        return std::unexpected(parsed_window.error());
                    }
                    active_windows.push_back(*parsed_window);
                }
            }

            return TriggerDefinition{
                .type = trigger_type::interval,
                .every = *every,
                .jitter = jitter,
                .time_zone = *time_zone,
                .active_windows = std::move(active_windows),
            };
        }

        if (*type_value == "once") {
            const auto at_value = parse_required_string_field(value, "at");
            if (!at_value.has_value()) {
                return std::unexpected(at_value.error());
            }

            const auto at = parse_iso_utc(*at_value);
            if (!at.has_value()) {
                return std::unexpected(at.error());
            }

            return TriggerDefinition{
                .type = trigger_type::once,
                .at = *at,
                .time_zone = "UTC",
            };
        }

        return std::unexpected("unsupported trigger type: " + *type_value);
    }

    std::expected<std::chrono::seconds, std::string> parse_duration_string(std::string_view value) {
        if (value.size() < 2) {
            return std::unexpected("duration must include a numeric value and unit suffix");
        }

        long long numeric = 0;
        const auto number_part = value.substr(0, value.size() - 1);
        auto [ptr, ec] = std::from_chars(number_part.begin(), number_part.end(), numeric);
        if (ec != std::errc{} || ptr != number_part.end() || numeric < 0) {
            return std::unexpected("duration must start with a non-negative integer");
        }

        switch (value.back()) {
        case 's':
            return std::chrono::seconds{numeric};
        case 'm':
            return std::chrono::minutes{numeric};
        case 'h':
            return std::chrono::hours{numeric};
        case 'd':
            return std::chrono::hours{numeric * 24};
        default:
            return std::unexpected("duration must end with s, m, h, or d");
        }
    }

} // namespace orangutan::automation
