#include "automation/parser.hpp"

#include "automation/cron-parser.hpp"
#include "utils/time-format.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace orangutan::automation {
    namespace {

        [[nodiscard]]
        std::expected<std::string, std::string> validate_time_zone_string(std::string_view value) {
            if (value.empty()) {
                return std::unexpected("time_zone must not be empty");
            }

            const auto is_blank = std::ranges::all_of(value, [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
            if (is_blank) {
                return std::unexpected("time_zone must not be empty");
            }

            if (value == "UTC") {
                return std::string(value);
            }

            try {
                static_cast<void>(std::chrono::locate_zone(std::string(value)));
            } catch (const std::runtime_error &) {
                return std::unexpected("time_zone must be UTC or a valid IANA zone name");
            }

            return std::string(value);
        }

        constexpr auto FULL_DAY_MINUTES = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours{24});
        constexpr std::array CRON_TRIGGER_FIELDS{
            std::string_view{"type"},
            std::string_view{"cron"},
            std::string_view{"time_zone"},
        };
        constexpr std::array INTERVAL_TRIGGER_FIELDS{
            std::string_view{"type"}, std::string_view{"every"}, std::string_view{"jitter"}, std::string_view{"time_zone"}, std::string_view{"active_windows"},
        };
        constexpr std::array ONCE_TRIGGER_FIELDS{
            std::string_view{"type"},
            std::string_view{"at"},
        };

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

            return validate_time_zone_string(field->get<std::string>());
        }

        template <std::size_t N>
        std::expected<void, std::string> validate_allowed_trigger_fields(const nlohmann::json &value, std::string_view trigger_name,
                                                                         const std::array<std::string_view, N> &allowed_fields) {
            for (const auto &entry : value.items()) {
                const auto field_name = std::string_view(entry.key());
                const auto allowed = std::ranges::find(allowed_fields, field_name) != allowed_fields.end();
                if (!allowed) {
                    return std::unexpected(std::string(trigger_name) + " trigger does not accept field: " + entry.key());
                }
            }
            return {};
        }

        [[nodiscard]]
        std::expected<std::chrono::seconds, std::string> seconds_from_numeric(long long numeric, long long multiplier) {
            if (numeric > std::numeric_limits<long long>::max() / multiplier) {
                return std::unexpected("duration is too large");
            }
            return std::chrono::seconds{numeric * multiplier};
        }

        [[nodiscard]]
        std::string format_duration_string(std::chrono::seconds value) {
            using namespace std::chrono;
            if (value != 0s) {
                if (value % days{1} == 0s) {
                    return std::to_string(value / days{1}) + "d";
                }
                if (value % 1h == 0s) {
                    return std::to_string(value / 1h) + "h";
                }
                if (value % 1min == 0s) {
                    return std::to_string(value / 1min) + "m";
                }
            }
            return std::to_string(value.count()) + "s";
        }

        [[nodiscard]]
        std::string format_time_of_day(std::chrono::minutes value) {
            return fmt::format("{:%H:%M}", value);
        }

        [[nodiscard]]
        std::expected<std::chrono::minutes, std::string> parse_time_of_day(std::string_view value) {
            if (value.size() != 5 || value[2] != ':') {
                return std::unexpected("time values must use hh:mm");
            }

            const auto hour = utils::parse_fixed_int(value, 0, 2);
            const auto minute = utils::parse_fixed_int(value, 3, 2);
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
        std::expected<TimePoint, std::string> parse_iso_utc(std::string_view value) {
            const auto parsed = utils::parse_iso8601_utc(value);
            if (!parsed.has_value()) {
                return std::unexpected("at must use iso-8601 utc format");
            }
            return std::chrono::time_point_cast<Clock::duration>(*parsed);
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
                    {"at", utils::format_iso8601_utc(trigger.at)},
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
            const auto allowed_fields = validate_allowed_trigger_fields(value, "cron", CRON_TRIGGER_FIELDS);
            if (!allowed_fields.has_value()) {
                return std::unexpected(allowed_fields.error());
            }

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
            const auto allowed_fields = validate_allowed_trigger_fields(value, "interval", INTERVAL_TRIGGER_FIELDS);
            if (!allowed_fields.has_value()) {
                return std::unexpected(allowed_fields.error());
            }

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

            const auto jitter_value = parse_required_string_field(value, "jitter");
            if (!jitter_value.has_value()) {
                return std::unexpected(jitter_value.error());
            }

            const auto jitter = parse_duration_string(*jitter_value);
            if (!jitter.has_value()) {
                return std::unexpected(jitter.error());
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
                .jitter = *jitter,
                .time_zone = *time_zone,
                .active_windows = std::move(active_windows),
            };
        }

        if (*type_value == "once") {
            const auto allowed_fields = validate_allowed_trigger_fields(value, "once", ONCE_TRIGGER_FIELDS);
            if (!allowed_fields.has_value()) {
                return std::unexpected(allowed_fields.error());
            }

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
                return seconds_from_numeric(numeric, 1);
            case 'm':
                return seconds_from_numeric(numeric, 60);
            case 'h':
                return seconds_from_numeric(numeric, static_cast<long long>(60) * 60);
            case 'd':
                return seconds_from_numeric(numeric, static_cast<long long>(24) * 60 * 60);
            default:
                return std::unexpected("duration must end with s, m, h, or d");
        }
    }

} // namespace orangutan::automation
