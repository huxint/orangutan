#pragma once

#include <iterator>
#include <string>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/fmt/bundled/color.h>
#include <spdlog/fmt/bundled/format.h>

namespace orangutan::utils {

    /// Format text into a string without exposing spdlog::fmt_lib details.
    template <typename... Args>
    void format_to(std::string &out, spdlog::fmt_lib::format_string<Args...> format_str, Args &&...args) {
        spdlog::fmt_lib::format_to(std::back_inserter(out), format_str, std::forward<Args>(args)...);
    }

    /// Format styled text into a string without exposing spdlog::fmt_lib details.
    template <typename... Args>
    void format_to(std::string &out, const spdlog::fmt_lib::text_style &style, spdlog::fmt_lib::format_string<Args...> format_str, Args &&...args) {
        spdlog::fmt_lib::format_to(std::back_inserter(out), style, format_str, std::forward<Args>(args)...);
    }

} // namespace orangutan::utils
