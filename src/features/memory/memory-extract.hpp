#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory_detail {

struct AutoCandidate {
    std::string key;
    std::string content;
    std::string category;
    double importance = 0.5;
};

/// Truncate a string at the last valid UTF-8 code point boundary.
/// Strips any trailing incomplete multibyte sequence so the result is always valid UTF-8.
[[nodiscard]]
std::string sanitize_utf8(std::string_view input);

[[nodiscard]]
std::vector<AutoCandidate> extract_auto_candidates(const std::string &text);
[[nodiscard]]
bool should_merge_auto_candidate(std::string_view key);
[[nodiscard]]
bool should_attempt_auto_capture(const std::string &text);

} // namespace orangutan::memory_detail
