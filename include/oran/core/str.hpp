// include/oran/core/str.hpp — UTF-8 boundary helpers.
//
// `oran-core` exposes the minimum UTF-8 primitives every higher layer keeps
// reaching for: a strict RFC-3629 validator, a code-point counter, and a
// boundary-safe byte-truncation helper. Higher-level work (normalization,
// case folding, grapheme clustering) is intentionally out of scope.
//
// The functions are `noexcept` and operate on `std::string_view`; they never
// allocate.

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace orangutan::core::str {

/// True iff `text` is well-formed UTF-8 (RFC 3629). Rejects overlong
/// encodings, lone continuation bytes, truncated multi-byte sequences, and
/// the UTF-16 surrogate range U+D800..U+DFFF.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// Number of Unicode code points in `text`. Returns `std::nullopt` when
/// `text` is not valid UTF-8 — use `is_valid_utf8` first if the caller wants
/// to distinguish empty input from invalid input by call.
[[nodiscard]] std::optional<std::size_t> count_code_points(std::string_view text) noexcept;

/// Largest prefix of `text` that fits in `max_bytes` and ends on a UTF-8
/// code-point boundary. The function only inspects byte indices; if `text`
/// is not valid UTF-8 the result is the longest prefix that does not split
/// what *looks like* a multi-byte lead. Callers that need a validated
/// prefix should `is_valid_utf8` first.
[[nodiscard]] std::string_view truncate_to_code_point(std::string_view text, std::size_t max_bytes) noexcept;

}  // namespace orangutan::core::str
