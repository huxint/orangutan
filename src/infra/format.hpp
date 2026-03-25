#pragma once

#include <fmt/format.h>
#include <string>

namespace orangutan {

/// Append formatted text to a string (replaces verbose std::format_to + std::back_inserter).
template <typename... Args>
void append(std::string &out, fmt::format_string<Args...> format_str, Args &&...args) {
    fmt::format_to(std::back_inserter(out), format_str, std::forward<Args>(args)...);
}

} // namespace orangutan
