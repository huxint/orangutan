#pragma once

#include <string_view>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string_view trim_copy(std::string_view value) {
        constexpr std::string_view blanks = " \t\n\r\f\v";
        const auto first = value.find_first_not_of(blanks);
        if (first == std::string_view::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(blanks);
        return value.substr(first, last - first + 1);
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::trim_copy;

} // namespace orangutan
