#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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
        std::string lowered;
        lowered.reserve(value.size());
        for (const unsigned char ch : value) {
            if (ch >= 'A' && ch <= 'Z') {
                lowered.push_back(static_cast<char>(ch - 'A' + 'a'));
                continue;
            }
            lowered.push_back(static_cast<char>(ch));
        }
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
    inline std::vector<std::string> split_csv_trimmed(std::string_view value) {
        std::vector<std::string> values;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto end = value.find(',', start);
            const auto token = end == std::string_view::npos ? value.substr(start) : value.substr(start, end - start);
            const auto trimmed = trim_copy(token);
            if (!trimmed.empty()) {
                values.emplace_back(trimmed);
            }

            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }

        return values;
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::ascii_to_lower_copy;
    using utils::normalize_enum_token;
    using utils::split_csv_trimmed;
    using utils::trim_copy;

} // namespace orangutan
