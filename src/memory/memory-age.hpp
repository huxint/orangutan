#pragma once

#include <string>
#include <string_view>

namespace orangutan::memory {

    /// Compute days elapsed since the given datetime string (SQLite format "YYYY-MM-DD HH:MM:SS").
    /// Returns 0 for today, negative if parsing fails.
    [[nodiscard]]
    int memory_age_days(std::string_view updated_at);

    /// Human-readable age: "today", "yesterday", "3 days ago", "2 weeks ago", "47 days ago".
    [[nodiscard]]
    std::string memory_age_text(std::string_view updated_at);

    /// Returns a staleness caveat for memories older than 1 day, empty otherwise.
    /// e.g. "(3 days ago — verify before acting on this)"
    [[nodiscard]]
    std::string memory_freshness_caveat(std::string_view updated_at);

} // namespace orangutan::memory

namespace orangutan {

    using memory::memory_age_days;
    using memory::memory_age_text;
    using memory::memory_freshness_caveat;

} // namespace orangutan
