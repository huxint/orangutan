#pragma once

#include <format>
#include <string>

namespace orangutan {

/// Append formatted text to a string (replaces verbose std::format_to + std::back_inserter).
template <typename... Args>
void append(std::string &out, std::format_string<Args...> fmt, Args &&...args) {
    std::format_to(std::back_inserter(out), fmt, std::forward<Args>(args)...);
}

} // namespace orangutan
