#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::utils {

    template <typename Writer>
    [[nodiscard]]
    inline std::string overwrite_string(std::size_t max_size, Writer &&writer) {
        std::string out;
        out.resize_and_overwrite(max_size, std::forward<Writer>(writer));
        return out;
    }

    [[nodiscard]]
    constexpr inline char ascii_to_lower_char(unsigned char ch) noexcept {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    }

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
        return overwrite_string(value.size(), [value](char *buffer, std::size_t size) {
            for (std::size_t index = 0; index < size; ++index) {
                buffer[index] = ascii_to_lower_char(static_cast<unsigned char>(value[index]));
            }
            return size;
        });
    }

    [[nodiscard]]
    inline std::string normalize_enum_token(std::string_view value) {
        return overwrite_string(value.size(), [value](char *buffer, std::size_t size) {
            for (std::size_t index = 0; index < size; ++index) {
                const auto ch = static_cast<unsigned char>(value[index]);
                buffer[index] = ch == '-' || ch == '_' ? '_' : ascii_to_lower_char(ch);
            }
            return size;
        });
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

    using utils::ascii_to_lower_char;
    using utils::ascii_to_lower_copy;
    using utils::normalize_enum_token;
    using utils::overwrite_string;
    using utils::split_csv_trimmed;
    using utils::trim_copy;

} // namespace orangutan
