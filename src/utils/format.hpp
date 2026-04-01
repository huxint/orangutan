#pragma once

#include <spdlog/common.h>
#include <string>

namespace orangutan::utils {

    /// Append formatted text to a string (replaces verbose std::format_to + std::back_inserter).
    template <typename... Args>
    void append(std::string &out, spdlog::fmt_lib::format_string<Args...> format_str, Args &&...args) {
        spdlog::fmt_lib::format_to(std::back_inserter(out), format_str, std::forward<Args>(args)...);
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::append;

} // namespace orangutan
