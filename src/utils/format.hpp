#pragma once

#include <iterator>
#include <string>
#include <utility>

#include <fmt/color.h>
#include <fmt/format.h>

namespace orangutan::utils {

    /// Append formatted text to `out`.
    template <typename... Args>
    void format_to(std::string &out, fmt::format_string<Args...> fmt_str, Args &&...args) {
        fmt::format_to(std::back_inserter(out), fmt_str, std::forward<Args>(args)...);
    }

    /// Append styled formatted text to `out`.
    template <typename... Args>
    void format_to(std::string &out, const fmt::text_style &style, fmt::format_string<Args...> fmt_str, Args &&...args) {
        fmt::format_to(std::back_inserter(out), style, fmt_str, std::forward<Args>(args)...);
    }

} // namespace orangutan::utils
