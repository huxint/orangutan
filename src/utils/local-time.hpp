#pragma once

#include <chrono>
#include <ctime>
#include <stdexcept>

namespace orangutan::time {
    namespace detail {

        [[nodiscard]]
        inline std::tm local_tm_from(std::chrono::system_clock::time_point tp) {
            const auto time = std::chrono::system_clock::to_time_t(tp);
            std::tm result{};
#if defined(_WIN32)
            if (localtime_s(&result, &time) != 0) {
                throw std::runtime_error("failed to convert system time to local time");
            }
#else
            if (localtime_r(&time, &result) == nullptr) {
                throw std::runtime_error("failed to convert system time to local time");
            }
#endif
            return result;
        }

    } // namespace detail

    [[nodiscard]]
    inline std::chrono::local_seconds local_time_from(std::chrono::system_clock::time_point tp) {
        const auto truncated_tp = std::chrono::floor<std::chrono::seconds>(tp);
        try {
            return std::chrono::zoned_time{std::chrono::current_zone(), truncated_tp}.get_local_time();
        } catch (const std::runtime_error &) {
            const auto local_tm = detail::local_tm_from(truncated_tp);
            const auto local_day = std::chrono::local_days{std::chrono::year{local_tm.tm_year + 1900} / std::chrono::month{static_cast<unsigned>(local_tm.tm_mon + 1)} /
                                                           std::chrono::day{static_cast<unsigned>(local_tm.tm_mday)}};
            return local_day + std::chrono::hours{local_tm.tm_hour} + std::chrono::minutes{local_tm.tm_min} + std::chrono::seconds{local_tm.tm_sec};
        }
    }

    [[nodiscard]]
    inline std::chrono::system_clock::time_point sys_time_from_local(std::chrono::local_seconds local_tp) {
        const auto local_day = std::chrono::floor<std::chrono::days>(local_tp);
        const auto ymd = std::chrono::year_month_day{local_day};
        const auto tod = std::chrono::hh_mm_ss{local_tp - local_day};
        if (!ymd.ok()) {
            throw std::runtime_error("invalid local date-time components");
        }

        try {
            return std::chrono::current_zone()->to_sys(local_tp, std::chrono::choose::latest);
        } catch (const std::runtime_error &) {
            std::tm local_tm{};
            local_tm.tm_year = static_cast<int>(ymd.year()) - 1900;
            local_tm.tm_mon = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
            local_tm.tm_mday = static_cast<int>(static_cast<unsigned>(ymd.day()));
            local_tm.tm_hour = static_cast<int>(tod.hours().count());
            local_tm.tm_min = static_cast<int>(tod.minutes().count());
            local_tm.tm_sec = static_cast<int>(tod.seconds().count());
            local_tm.tm_isdst = -1;
            return std::chrono::system_clock::from_time_t(std::mktime(&local_tm));
        }
    }

} // namespace orangutan::time
