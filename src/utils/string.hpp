#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <ctre.hpp>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string_view trim_copy(std::string_view value) {
        constexpr std::string_view BLANKS = " \t\n\r\f\v";
        const auto first = value.find_first_not_of(BLANKS);
        if (first == std::string_view::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(BLANKS);
        return value.substr(first, last - first + 1);
    }

    [[nodiscard]]
    inline std::string ascii_to_lower_copy(std::string_view value) {
        std::string lowered(value.size(), '\0');
        std::ranges::transform(value, lowered.begin(), [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
        });
        return lowered;
    }

    [[nodiscard]]
    inline std::string normalize_enum_token(std::string_view value) {
        std::string normalized;
        normalized.reserve(value.size());
        for (const unsigned char ch : value) {
            if (ch == '-' || ch == '_') {
                normalized.push_back('_');
                continue;
            }

            if (ch >= 'A' && ch <= 'Z') {
                normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
                continue;
            }

            normalized.push_back(static_cast<char>(ch));
        }
        return normalized;
    }

    [[nodiscard]]
    inline std::vector<std::string> split_trimmed(std::string_view value) {
        std::vector<std::string> values;
        for (const auto &match : ctre::search_all<R"(\s*([^,\s](?:[^,]*[^,\s])?)\s*(?:,|$))">(value)) {
            values.emplace_back(match.template get<1>().to_view());
        }
        return values;
    }

    /// Split on '\n'. Matches std::getline semantics:
    ///   - empty input yields an empty vector
    ///   - trailing '\n' does not yield an empty final line
    ///   - '\r' is preserved verbatim; strip it at the call site if DOS-compat is desired
    [[nodiscard]]
    inline std::vector<std::string> split_lines(std::string_view value) {
        std::vector<std::string> lines;
        for (std::size_t pos = 0; pos < value.size();) {
            const auto next = value.find('\n', pos);
            const auto end = next == std::string_view::npos ? value.size() : next;
            lines.emplace_back(value.substr(pos, end - pos));
            if (next == std::string_view::npos) {
                break;
            }
            pos = next + 1;
        }
        return lines;
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::ascii_to_lower_copy;
    using utils::normalize_enum_token;
    using utils::split_lines;
    using utils::split_trimmed;
    using utils::trim_copy;

} // namespace orangutan
