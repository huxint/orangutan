#include "automation/parser.hpp"

#include "automation/cron-parser.hpp"
#include "utils/expected-combine.hpp"
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
            const auto field = value.find(field_name);
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

            auto parts = utils::all_ok(
                parse_required_string_field(value, "start").and_then(parse_time_of_day),
                parse_required_string_field(value, "end").and_then(parse_time_of_day));
            if (!parts.has_value()) {
                return std::unexpected(parts.error());
            }
            const auto &[start, end] = *parts;

            if (start >= end) {
                return std::unexpected("active window start must be before end");
            }

            return ActiveWindow{
                .start = start,
                .end = end,
            };
        }

        [[nodiscard]]
        std::expected<std::vector<ActiveWindow>, std::string> parse_active_windows(const nlohmann::json &value) {
            std::vector<ActiveWindow> result;
            const auto field = value.find("active_windows");
            if (field == value.end()) {
                return result;
            }
            if (!field->is_array()) {
                return std::unexpected("active_windows must be an array");
            }
            for (const auto &window_value : *field) {
                auto parsed = parse_active_window(window_value);
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed).error());
                }
                result.push_back(*std::move(parsed));
            }
            return result;
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
            auto cron_str = parse_required_string_field(value, "cron");
            auto parts = utils::all_ok(
                validate_allowed_trigger_fields(value, "cron", CRON_TRIGGER_FIELDS),
                cron_str,
                cron_str.and_then(parse_cron_expression),
                parse_time_zone_field(value));
            if (!parts.has_value()) {
                return std::unexpected(parts.error());
            }
            auto &[_allowed, cron_value, _parsed_cron, time_zone] = *parts;

            return TriggerDefinition{
                .type = trigger_type::cron,
                .cron = std::move(cron_value),
                .time_zone = std::move(time_zone),
            };
        }

        if (*type_value == "interval") {
            auto every = parse_required_string_field(value, "every")
                             .and_then(parse_duration_string)
                             .and_then([](std::chrono::seconds d) -> std::expected<std::chrono::seconds, std::string> {
                                 if (d <= std::chrono::seconds{0}) {
                                     return std::unexpected("every must be greater than zero");
                                 }
                                 return d;
                             });
            auto jitter = parse_required_string_field(value, "jitter").and_then(parse_duration_string);

            auto parts = utils::all_ok(
                validate_allowed_trigger_fields(value, "interval", INTERVAL_TRIGGER_FIELDS),
                every,
                jitter,
                parse_time_zone_field(value),
                parse_active_windows(value));
            if (!parts.has_value()) {
                return std::unexpected(parts.error());
            }
            auto &[_allowed, every_value, jitter_value, time_zone, active_windows] = *parts;

            return TriggerDefinition{
                .type = trigger_type::interval,
                .every = every_value,
                .jitter = jitter_value,
                .time_zone = std::move(time_zone),
                .active_windows = std::move(active_windows),
            };
        }

        if (*type_value == "once") {
            auto parts = utils::all_ok(
                validate_allowed_trigger_fields(value, "once", ONCE_TRIGGER_FIELDS),
                parse_required_string_field(value, "at").and_then(parse_iso_utc));
            if (!parts.has_value()) {
                return std::unexpected(parts.error());
            }
            const auto &[_allowed, at] = *parts;

            return TriggerDefinition{
                .type = trigger_type::once,
                .at = at,
                .time_zone = "UTC",
            };
        }

        return std::unexpected("unsupported trigger type: " + *type_value);
    }

    std::expected<std::chrono::seconds, std::string> parse_duration_string(std::string_view value) {
        using namespace std::chrono;

        if (value.size() < 2) {
            return std::unexpected("duration must include a numeric value and unit suffix");
        }

        seconds unit{};
        switch (value.back()) {
            case 's':
                unit = 1s;
                break;
            case 'm':
                unit = 1min;
                break;
            case 'h':
                unit = 1h;
                break;
            case 'd':
                unit = 24h;
                break;
            default:
                return std::unexpected("duration must end with s, m, h, or d");
        }

        const auto digits = value.substr(0, value.size() - 1);
        seconds::rep count = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
        if (ec != std::errc{} || ptr != digits.data() + digits.size() || count < 0) {
            return std::unexpected("duration must start with a non-negative integer");
        }

        if (count > std::numeric_limits<seconds::rep>::max() / unit.count()) {
            return std::unexpected("duration is too large");
        }

        return unit * count;
    }

} // namespace orangutan::automation
