#pragma once

#include "utils/format.hpp"
#include "utils/local-time.hpp"

#include <spdlog/fmt/chrono.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string format_local_date(std::chrono::system_clock::time_point tp) {
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), tp}.get_local_time();
            return utils::format("{:%Y-%m-%d}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(tp);
            return utils::format("{:04}-{:02}-{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);
        }
    }

    [[nodiscard]]
    inline std::string format_local_timestamp(std::chrono::system_clock::time_point tp) {
        const auto truncated_tp = std::chrono::floor<std::chrono::seconds>(tp);
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), truncated_tp}.get_local_time();
            return utils::format("{:%Y-%m-%d %H:%M:%S}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(truncated_tp);
            return utils::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min,
                                 local_tm.tm_sec);
        }
    }

    [[nodiscard]]
    inline std::string format_local_iso8601_timestamp(std::chrono::system_clock::time_point tp) {
        const auto truncated_tp = std::chrono::floor<std::chrono::seconds>(tp);
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), truncated_tp}.get_local_time();
            return utils::format("{:%Y-%m-%dT%H:%M:%S}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(truncated_tp);
            return utils::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min,
                                 local_tm.tm_sec);
        }
    }

    [[nodiscard]]
    inline std::string current_local_date() {
        return format_local_date(std::chrono::system_clock::now());
    }

    [[nodiscard]]
    inline std::string current_local_timestamp() {
        return format_local_timestamp(std::chrono::system_clock::now());
    }

    [[nodiscard]]
    inline std::string current_local_iso8601_timestamp() {
        return format_local_iso8601_timestamp(std::chrono::system_clock::now());
    }

    [[nodiscard]]
    inline std::int64_t epoch_millis(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    }

    [[nodiscard]]
    inline std::int64_t steady_micros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]]
    inline std::optional<int> parse_fixed_int(std::string_view value, std::size_t offset, std::size_t width) {
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
    inline std::string format_iso8601_utc(std::chrono::system_clock::time_point tp) {
        const auto truncated = std::chrono::floor<std::chrono::seconds>(tp);
        const auto day = std::chrono::floor<std::chrono::days>(truncated);
        const auto ymd = std::chrono::year_month_day{day};
        if (!ymd.ok()) {
            return {};
        }

        const auto tod = std::chrono::hh_mm_ss{truncated - day};
        return utils::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                             static_cast<int>(ymd.year()),
                             static_cast<unsigned>(ymd.month()),
                             static_cast<unsigned>(ymd.day()),
                             static_cast<int>(tod.hours().count()),
                             static_cast<int>(tod.minutes().count()),
                             static_cast<int>(tod.seconds().count()));
    }

    [[nodiscard]]
    inline std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(std::string_view iso) {
        if (iso.size() != 20 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' || iso[19] != 'Z') {
            return std::nullopt;
        }

        const auto year = parse_fixed_int(iso, 0, 4);
        const auto month = parse_fixed_int(iso, 5, 2);
        const auto day = parse_fixed_int(iso, 8, 2);
        const auto hour = parse_fixed_int(iso, 11, 2);
        const auto minute = parse_fixed_int(iso, 14, 2);
        const auto second = parse_fixed_int(iso, 17, 2);
        if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value()) {
            return std::nullopt;
        }
        if (*hour > 23 || *minute > 59 || *second > 59) {
            return std::nullopt;
        }

        const auto ymd = std::chrono::year{*year} / std::chrono::month{static_cast<unsigned>(*month)} / std::chrono::day{static_cast<unsigned>(*day)};
        if (!ymd.ok()) {
            return std::nullopt;
        }

        const auto utc_time = std::chrono::sys_days{ymd} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(utc_time);
    }

} // namespace orangutan::utils
