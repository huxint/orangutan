#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan {

struct Anchor {
    size_t line;
    std::string hash;
};

struct HashMismatch {
    size_t line;
    std::string expected;
    std::string actual;
};

// Compute 2-char hash for a line using xxHash32.
// Symbol-only lines (no alphanumeric chars) use line_number as seed;
// all other lines use seed=0.
std::string compute_line_hash(std::string_view line, size_t line_number);

// Format a line with hash tag: "LINE#HASH:content"
std::string format_hashline(std::string_view line, size_t line_number);

// Parse an anchor string "LINE#HASH" into an Anchor struct.
// Throws std::runtime_error on invalid format.
Anchor parse_anchor(std::string_view anchor_str);

// Validate an anchor against a vector of lines.
// Returns std::nullopt if valid, or a HashMismatch describing the problem.
std::optional<HashMismatch> validate_anchor(const Anchor &anchor,
                                             const std::vector<std::string> &lines);

} // namespace orangutan
