#pragma once

#include <spdlog/common.h>
#include <string>

namespace orangutan::utils {

    /// Format text into a string without exposing spdlog::fmt_lib details.
    template <typename... Args>
    void format_to(std::string &out, spdlog::fmt_lib::format_string<Args...> format_str, Args &&...args) {
        spdlog::fmt_lib::format_to(std::back_inserter(out), format_str, std::forward<Args>(args)...);
    }

} // namespace orangutan::utils
