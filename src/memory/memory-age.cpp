#include "memory/memory-age.hpp"

#include <charconv>
#include <chrono>
#include <spdlog/common.h>

namespace orangutan::memory {

    namespace {

        /// Parse "YYYY-MM-DD HH:MM:SS" into a system_clock time_point.
        /// Returns epoch on failure.
        std::chrono::system_clock::time_point parse_sqlite_datetime(std::string_view value) {
            if (value.size() < 19) {
                return {};
            }

            auto parse_int = [](std::string_view sv) -> unsigned {
                unsigned result = 0;
                std::from_chars(sv.begin(), sv.end(), result);
                return result;
            };

            return std::chrono::sys_days{std::chrono::year{static_cast<int>(parse_int(value.substr(0, 4)))} / std::chrono::month{parse_int(value.substr(5, 2))} /
                                         std::chrono::day{parse_int(value.substr(8, 2))}} +
                   std::chrono::hours{parse_int(value.substr(11, 2))} + std::chrono::minutes{parse_int(value.substr(14, 2))} + std::chrono::seconds{parse_int(value.substr(17, 2))};
        }

    } // namespace

    int memory_age_days(std::string_view updated_at) {
        const auto diff = std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now() - parse_sqlite_datetime(updated_at)).count();
        return static_cast<int>(diff / 24);
    }

    std::string memory_age_text(std::string_view updated_at) {
        const auto days = memory_age_days(updated_at);
        if (days < 0) {
            return "unknown age";
        }
        if (days == 0) {
            return "today";
        }
        if (days == 1) {
            return "yesterday";
        }
        if (days < 7) {
            return spdlog::fmt_lib::format("{} days ago", days);
        }
        if (days < 14) {
            return "1 week ago";
        }
        if (days < 30) {
            return spdlog::fmt_lib::format("{} weeks ago", days / 7);
        }
        if (days < 60) {
            return "1 month ago";
        }
        return spdlog::fmt_lib::format("{} months ago", days / 30);
    }

    std::string memory_freshness_caveat(std::string_view updated_at) {
        const auto days = memory_age_days(updated_at);
        if (days <= 1) {
            return {};
        }
        return spdlog::fmt_lib::format("({} — verify before acting on this)", memory_age_text(updated_at));
    }

} // namespace orangutan::memory
