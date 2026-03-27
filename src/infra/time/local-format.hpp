#pragma once

#include "infra/time/local-time.hpp"

#include <spdlog/common.h>
#include <spdlog/fmt/chrono.h>
#include <string>

namespace orangutan::time {

    [[nodiscard]]
    inline std::string format_local_date(std::chrono::system_clock::time_point tp) {
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), tp}.get_local_time();
            return spdlog::fmt_lib::format("{:%Y-%m-%d}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(tp);
            return spdlog::fmt_lib::format("{:04}-{:02}-{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);
        }
    }

    [[nodiscard]]
    inline std::string format_local_timestamp(std::chrono::system_clock::time_point tp) {
        const auto truncated_tp = std::chrono::floor<std::chrono::seconds>(tp);
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), truncated_tp}.get_local_time();
            return spdlog::fmt_lib::format("{:%Y-%m-%d %H:%M:%S}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(truncated_tp);
            return spdlog::fmt_lib::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min,
                                           local_tm.tm_sec);
        }
    }

    [[nodiscard]]
    inline std::string format_local_iso8601_timestamp(std::chrono::system_clock::time_point tp) {
        const auto truncated_tp = std::chrono::floor<std::chrono::seconds>(tp);
        try {
            const auto lt = std::chrono::zoned_time{std::chrono::current_zone(), truncated_tp}.get_local_time();
            return spdlog::fmt_lib::format("{:%Y-%m-%dT%H:%M:%S}", lt);
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(truncated_tp);
            return spdlog::fmt_lib::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min,
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

} // namespace orangutan::time
