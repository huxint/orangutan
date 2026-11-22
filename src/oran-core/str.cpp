// src/oran-core/str.cpp — UTF-8 boundary helpers (RFC 3629).

#include <oran/core/str.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace orangutan::core::str {

namespace {

// Classifies an opening byte. Returns the total sequence length (1..4) or 0
// if the byte cannot begin a valid UTF-8 sequence.
[[nodiscard]] constexpr int lead_length(std::uint8_t b) noexcept {
  if (b < 0x80) {
    return 1;
  }
  if (b < 0xC2) {
    return 0;  // 0x80-0xBF: lone continuation; 0xC0-0xC1: overlong.
  }
  if (b < 0xE0) {
    return 2;
  }
  if (b < 0xF0) {
    return 3;
  }
  if (b < 0xF5) {
    return 4;
  }
  return 0;  // 0xF5-0xFF: never valid (codepoint > U+10FFFF).
}

// Per RFC 3629 the allowed continuation range narrows for some leads:
//   0xE0      → first continuation must be 0xA0..0xBF (reject overlong)
//   0xED      → first continuation must be 0x80..0x9F (reject surrogates)
//   0xF0      → first continuation must be 0x90..0xBF (reject overlong)
//   0xF4      → first continuation must be 0x80..0x8F (cap at U+10FFFF)
// All other continuations must be in 0x80..0xBF.
struct ContRange {
  std::uint8_t lo;
  std::uint8_t hi;
};

[[nodiscard]] constexpr ContRange first_continuation_range(std::uint8_t lead) noexcept {
  switch (lead) {
    case 0xE0:
      return {0xA0, 0xBF};
    case 0xED:
      return {0x80, 0x9F};
    case 0xF0:
      return {0x90, 0xBF};
    case 0xF4:
      return {0x80, 0x8F};
    default:
      return {0x80, 0xBF};
  }
}

[[nodiscard]] constexpr bool in_range(std::uint8_t b, ContRange r) noexcept {
  return b >= r.lo && b <= r.hi;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    const std::uint8_t lead = *p;
    const int length = lead_length(lead);
    if (length == 0) {
      return false;
    }
    if (end - p < length) {
      return false;
    }
    if (length >= 2) {
      const auto r1 = first_continuation_range(lead);
      if (!in_range(p[1], r1)) {
        return false;
      }
      for (int i = 2; i < length; ++i) {
        if (!in_range(p[i], {0x80, 0xBF})) {
          return false;
        }
      }
    }
    p += length;
  }
  return true;
}

std::optional<std::size_t> count_code_points(std::string_view text) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
  const auto* end = p + text.size();
  std::size_t count = 0;
  while (p < end) {
    const std::uint8_t lead = *p;
    const int length = lead_length(lead);
    if (length == 0 || (end - p) < length) {
      return std::nullopt;
    }
    if (length >= 2) {
      const auto r1 = first_continuation_range(lead);
      if (!in_range(p[1], r1)) {
        return std::nullopt;
      }
      for (int i = 2; i < length; ++i) {
        if (!in_range(p[i], {0x80, 0xBF})) {
          return std::nullopt;
        }
      }
    }
    p += length;
    ++count;
  }
  return count;
}

std::string_view truncate_to_code_point(std::string_view text, std::size_t max_bytes) noexcept {
  if (text.size() <= max_bytes) {
    return text;
  }
  // Walk forward over complete code points and remember the last index that
  // still fits in `max_bytes`. The traversal is byte-driven; we do not need
  // to fully validate, only to avoid splitting what looks like a multi-byte
  // sequence by its declared length.
  const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
  std::size_t boundary = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const int length = lead_length(p[i]);
    const std::size_t step = (length == 0) ? 1U : static_cast<std::size_t>(length);
    if (i + step > max_bytes) {
      break;
    }
    if (i + step > text.size()) {
      break;
    }
    i += step;
    boundary = i;
  }
  return text.substr(0, boundary);
}

}  // namespace orangutan::core::str
