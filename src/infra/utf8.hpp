#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace orangutan::utf8 {

    /// Return valid UTF-8 by preserving valid spans, skipping malformed bytes,
    /// and dropping any trailing incomplete sequence.
    [[nodiscard]]
    std::string sanitize(std::string_view input);

    /// Sanitize malformed UTF-8, then truncate to at most max_bytes, optionally
    /// appending an ASCII ellipsis when truncation occurs. The returned prefix
    /// always ends on a code point boundary.
    [[nodiscard]]
    std::string sanitize_and_truncate_valid_prefix(std::string_view input, size_t max_bytes, bool append_ellipsis = false);

    /// Truncate a valid UTF-8 string to at most max_bytes, optionally appending
    /// an ASCII ellipsis when truncation occurs. The returned prefix always ends on
    /// a code point boundary.
    [[nodiscard]]
    std::string truncate_valid_prefix(std::string_view input, size_t max_bytes, bool append_ellipsis = false);

    /// Return the longest valid UTF-8 suffix whose byte length does not exceed
    /// max_bytes. The returned suffix always starts on a code point boundary.
    [[nodiscard]]
    std::string truncate_valid_suffix(std::string_view input, size_t max_bytes);

} // namespace orangutan::utf8
