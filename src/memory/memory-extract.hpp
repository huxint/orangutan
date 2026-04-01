#pragma once

#include "types/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory::detail {

    struct AutoCandidate {
        std::string key;
        std::string content;
        std::string category;
        base::f64 importance = 0.5;
    };

    [[nodiscard]]
    std::vector<AutoCandidate> extract_auto_candidates(const std::string &text);
    [[nodiscard]]
    bool should_merge_auto_candidate(std::string_view key);
    [[nodiscard]]
    bool should_attempt_auto_capture(const std::string &text);

} // namespace orangutan::memory::detail
