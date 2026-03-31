#pragma once

#include <cctype>
#include <ranges>
#include <string_view>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string_view trim_copy(std::string_view value) {
        const auto is_space = [](unsigned char c) {
            return std::isspace(c) != 0;
        };
        const auto *left = std::ranges::find_if_not(value, is_space);
        const auto *right = std::ranges::find_if_not(value | std::views::reverse, is_space).base();
        return left >= right ? std::string_view{} : std::string_view(left, right);
    }

} // namespace orangutan
