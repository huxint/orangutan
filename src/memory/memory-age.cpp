#include "memory/memory-age.hpp"

#include <fmt/format.h>

#include <charconv>
#include <chrono>
#include <optional>
#include <system_error>

namespace orangutan::memory {

    namespace {

        /// Parse "YYYY-MM-DD HH:MM:SS" into a system_clock time_point.
        [[nodiscard]]
        std::optional<std::chrono::system_clock::time_point> parse_sqlite_datetime(std::string_view value) {
            if (value.size() < 19) {
                return std::nullopt;
            }

            auto parse_int = [](std::string_view sv) -> std::optional<unsigned> {
                unsigned result = 0;
                const auto [ptr, ec] = std::from_chars(sv.begin(), sv.end(), result);
                if (ec != std::errc{} || ptr != sv.end()) {
                    return std::nullopt;
                }
                return result;
            };

            const auto year = parse_int(value.substr(0, 4));
            const auto month = parse_int(value.substr(5, 2));
            const auto day = parse_int(value.substr(8, 2));
            const auto hour = parse_int(value.substr(11, 2));
            const auto minute = parse_int(value.substr(14, 2));
            const auto second = parse_int(value.substr(17, 2));
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value()) {
                return std::nullopt;
            }

            const auto y = std::chrono::year{static_cast<int>(*year)};
            const auto m = std::chrono::month{*month};
            const auto d = std::chrono::day{*day};
            if (!y.ok() || !m.ok() || !d.ok() || *hour > 23 || *minute > 59 || *second > 59) {
                return std::nullopt;
            }

            return std::chrono::sys_days{y / m / d} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
        }

    } // namespace

    int memory_age_days(std::string_view updated_at) {
        const auto parsed = parse_sqlite_datetime(updated_at);
        if (!parsed.has_value()) {
            return -1;
        }

        const auto now = std::chrono::system_clock::now();
        if (*parsed > now) {
            return -1;
        }

        const auto diff = std::chrono::duration_cast<std::chrono::hours>(now - *parsed).count();
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
            return fmt::format("{} days ago", days);
        }
        if (days < 14) {
            return "1 week ago";
        }
        if (days < 30) {
            return fmt::format("{} weeks ago", days / 7);
        }
        if (days < 60) {
            return "1 month ago";
        }
        return fmt::format("{} months ago", days / 30);
    }

    std::string memory_freshness_caveat(std::string_view updated_at) {
        const auto days = memory_age_days(updated_at);
        if (days <= 1) {
            return {};
        }
        return fmt::format("({} — verify before acting on this)", memory_age_text(updated_at));
    }

} // namespace orangutan::memory
