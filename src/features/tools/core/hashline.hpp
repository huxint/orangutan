#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace orangutan {

// Compute 2-char hash for a line using xxHash32.
// Symbol-only lines (no alphanumeric chars) use line_number as seed;
// all other lines use seed=0.
std::string compute_line_hash(std::string_view line, size_t line_number);

// Format a line with hash tag: "LINE#HASH:content"
std::string format_hashline(std::string_view line, size_t line_number);

} // namespace orangutan
