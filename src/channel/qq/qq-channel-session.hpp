#pragma once

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <spdlog/fmt/bundled/format.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    [[nodiscard]]
    inline std::string getenv_or_default(const char *name, const char *fallback) {
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return fallback;
        }
        return value;
    }

    [[nodiscard]]
    inline std::string sanitize_path_component(std::string input) {
        for (auto &ch : input) {
            if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '-' && ch != '_') {
                ch = '_';
            }
        }
        if (input.empty()) {
            return "default";
        }
        return input;
    }

    [[nodiscard]]
    inline std::filesystem::path qq_session_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "sessions" / ("session-" + safe_name + ".json");
    }

    [[nodiscard]]
    inline std::filesystem::path qq_known_users_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "known-users" / ("known-users-" + safe_name + ".json");
    }

    [[nodiscard]]
    inline std::filesystem::path qq_ref_index_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "ref-index" / ("ref-index-" + safe_name + ".jsonl");
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
    inline std::string format_iso_utc(std::chrono::system_clock::time_point tp) {
        const auto truncated = std::chrono::floor<std::chrono::seconds>(tp);
        const auto day = std::chrono::floor<std::chrono::days>(truncated);
        const auto ymd = std::chrono::year_month_day{day};
        if (!ymd.ok()) {
            return {};
        }

        const auto tod = std::chrono::hh_mm_ss{truncated - day};
        return spdlog::fmt_lib::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                                       static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()), static_cast<int>(tod.seconds().count()));
    }

    [[nodiscard]]
    inline std::optional<std::chrono::system_clock::time_point> parse_iso_utc(std::string_view iso) {
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

} // namespace orangutan::channel::qq
