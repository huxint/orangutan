#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/str.hpp>

namespace orangutan::tool::detail {

inline constexpr auto kMemoryKinds =
    std::to_array<std::string_view>({"user", "feedback", "project", "reference", "team"});

[[nodiscard]] inline core::Result<void>
validate_memory_text(std::string_view text, std::string_view field, bool multiline = false) {
  const auto whitespace = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
  };
  if (std::ranges::all_of(text, whitespace) || !core::str::is_valid_utf8(text) ||
      std::ranges::any_of(text, [multiline, whitespace](char ch) {
        return static_cast<unsigned char>(ch) < 0x20U && !(multiline && whitespace(ch));
      })) {
    return std::unexpected(
        core::Error::invalid_argument("memory field must contain nonblank UTF-8 text without control characters")
            .with("field", std::string{field}));
  }
  return {};
}

}  // namespace orangutan::tool::detail
